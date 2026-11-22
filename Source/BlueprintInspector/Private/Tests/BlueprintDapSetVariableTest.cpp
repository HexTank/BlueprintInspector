// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

#include "Editor.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "UObject/UnrealType.h"

#include "BlueprintDapTestClient.h"
#include "BlueprintFixtureSource.h"
#include "BlueprintInspectorSubsystem.h"
#include "BlueprintScriptDisassembler.h"
#include "BlueprintScriptVisualizer_Pseudo.h"
#include "BlueprintSourceMap.h"
#include "BlueprintTestConfiguration.h"

#include "dap/network.h"
#include "dap/session.h"

#include <atomic>
#include <exception>
#include <memory>

namespace BlueprintDapSetVariableTest
{
using namespace BlueprintDapTest;

constexpr int32 ExpectedValue = 123;
constexpr int32 ExpectedArrayValue = 456;
const TCHAR* const FunctionName = TEXT("flow_switch");
const TCHAR* const VariableName = TEXT("IntVar");
// Request payloads use the same literals the assertions check, so either side cannot desync silently.
const TCHAR* const ExpectedValueText = TEXT("123");
const TCHAR* const ExpectedArrayValueText = TEXT("456");

struct FState : FClientState
{
    std::atomic<bool> bAuthenticated{false};
    std::atomic<bool> bStopped{false};
    std::atomic<bool> bCallInFlight{false};
    // Set once the exit guard owns the completion signal: Finish() then stops signalling bDone, and the
    // guard's destructor raises it after it has resumed/cleared. Otherwise the game thread can observe
    // bDone, join, and thereby prevent the guard's own requests from being served.
    std::atomic<bool> bDeferDone{false};
    std::atomic<bool> bSetVariable{false};
    std::atomic<bool> bArraySet{false};
    std::atomic<bool> bObjectSet{false};
    std::atomic<bool> bObjectCleared{false};
    std::atomic<int32> Value{0};
    std::atomic<int32> ArrayValue{0};
};

// One client per test run.
struct FFixture
{
    TSharedPtr<FState> State = MakeShared<FState>();

    bool bCallStarted = false;
    double PieWaitStart = -1.0;

    FWorker Worker;
};

// Resumes the VM (and, for the breakpoint client, clears the armed set) on every exit path after the
// connection is authenticated. The game thread parks inside ProcessEvent, so a worker that returns
// early on a request failure must not leave it there.
template <typename StateType>
struct FClientExitGuard
{
    dap::Session* Session = nullptr;
    StateType* State = nullptr;
    const std::atomic<bool>* Stop = nullptr;
    // Set only when the client reached a stop; a run that never parked must not send a continue.
    const std::atomic<bool>* Stopped = nullptr;
    const std::atomic<bool>* MayBeParked = nullptr;
    const dap::Source* BreakpointSource = nullptr;
    bool bBreakpointArmed = false;
    bool bResumed = false;
    bool bCleared = false;

    // Signalled last, after the cleanup requests have been answered. The game thread waits on this
    // flag and then joins the worker, so raising it any earlier would stall those requests.
    std::atomic<bool>* DoneFlag = nullptr;

    ~FClientExitGuard()
    {
        try
        {
            if (Session != nullptr && State != nullptr && Stop != nullptr)
            {
                const bool bObservedStop = Stopped != nullptr && Stopped->load();
                const bool bMayBeParked = bBreakpointArmed || (MayBeParked != nullptr && MayBeParked->load());
                if (!bResumed && (bObservedStop || bMayBeParked))
                {
                    bResumed = true;
                    dap::ContinueRequest Request;
                    Request.threadId = 1;
                    dap::ContinueResponse Response;
                    // Ignore the stop flag: the game thread may be parked until this request is served.
                    Send(*Session, Request, Response, *State, *Stop, TEXT("continue (exit)"), 5.0, true);
                }

                if (bBreakpointArmed && BreakpointSource != nullptr && !bCleared)
                {
                    bCleared = true;
                    dap::SetBreakpointsRequest Request;
                    Request.source = *BreakpointSource;
                    Request.breakpoints = dap::array<dap::SourceBreakpoint>{};
                    dap::SetBreakpointsResponse Response;
                    Send(*Session, Request, Response, *State, *Stop, TEXT("clear breakpoints (exit)"), 5.0, true);
                }
            }
        }
        catch (const std::exception& Exception)
        {
            // Send records timeouts and error responses, but cppdap can throw before it does; keep the cause.
            if (State != nullptr)
            {
                State->Fail(FString::Printf(TEXT("DAP cleanup threw: %s"), UTF8_TO_TCHAR(Exception.what())));
            }
        }
        catch (...)
        {
            if (State != nullptr)
            {
                State->Fail(TEXT("DAP cleanup threw an unknown exception."));
            }
        }

        if (DoneFlag != nullptr)
        {
            DoneFlag->store(true);
        }
    }
};

void RunClient(const FString& TokenPath, FFixture& Fixture)
{
    FState& State = *Fixture.State;
    const std::atomic<bool>& Stop = Fixture.Worker.Stop();

    // Once the exit guard owns completion (after authentication), it raises bDone after its own
    // continue/clear requests have been served.
    const auto Finish = [&State]() { if (!State.bDeferDone.load()) { State.bDone.store(true); } };

    std::shared_ptr<dap::ReaderWriter> ReaderWriter = Connect(Stop, State);
    if (!ReaderWriter)
    {
        Finish();
        return;
    }

    std::unique_ptr<dap::Session> Session = dap::Session::create();
    Session->registerHandler([&State](const dap::StoppedEvent&) { State.bStopped.store(true); });
    Session->bind(ReaderWriter, [&State]() {
        // ClosedHandler takes no message. The client closes its own session after the test;
        // a close before bDone means the transport/session failed mid-run.
        if (!State.bDone.load())
        {
            State.Fail(TEXT("The DAP session closed before the client finished."));
        }
    });

    FString Token;
    if (!ReadSessionToken(TokenPath, Stop, Token, State))
    {
        Finish();
        return;
    }

    dap::InitializeRequest InitRequest;
    InitRequest.adapterID = "blueprint-inspector";
    InitRequest.clientID = "BlueprintInspectorAutomation";
    InitRequest.linesStartAt1 = true;
    InitRequest.columnsStartAt1 = true;
    dap::InitializeResponse InitResponse;
    if (!Send(*Session, InitRequest, InitResponse, State, Stop, TEXT("initialize")))
    {
        Finish();
        return;
    }

    dap::LaunchRequest LaunchRequest;
    using RestartVariant = dap::variant<dap::array<dap::any>, dap::boolean, dap::integer, dap::null, dap::number, dap::object, dap::string>;
    dap::object Restart;
    Restart["token"] = dap::string(TCHAR_TO_UTF8(*Token));
    LaunchRequest.restart = RestartVariant(Restart);
    dap::LaunchResponse LaunchResponse;
    if (!Send(*Session, LaunchRequest, LaunchResponse, State, Stop, TEXT("launch")))
    {
        Finish();
        return;
    }
    State.bAuthenticated.store(true);
    FClientExitGuard<FState> ExitGuard{Session.get(), &State};
    ExitGuard.Stopped = &State.bStopped;
    ExitGuard.MayBeParked = &State.bCallInFlight;
    ExitGuard.Stop = &Stop;
    ExitGuard.DoneFlag = &State.bDone;
    State.bDeferDone.store(true);

    const double StopDeadline = FPlatformTime::Seconds() + 15.0;
    WaitWhileRunning(Stop, StopDeadline, [&State]() { return State.bStopped.load(); });
    if (!State.bStopped.load())
    {
        State.Fail(TEXT("The fixture never stopped at the armed breakpoint."));
        Finish();
        return;
    }

    dap::StackTraceRequest StackRequest;
    StackRequest.threadId = 1;
    dap::StackTraceResponse StackResponse;
    if (!Send(*Session, StackRequest, StackResponse, State, Stop, TEXT("stackTrace")) || StackResponse.stackFrames.empty())
    {
        State.Fail(TEXT("The paused frame had no stack trace."));
        Finish();
        return;
    }

    dap::ScopesRequest ScopesRequest;
    ScopesRequest.frameId = StackResponse.stackFrames[0].id;
    dap::ScopesResponse ScopesResponse;
    if (!Send(*Session, ScopesRequest, ScopesResponse, State, Stop, TEXT("scopes")))
    {
        Finish();
        return;
    }

    const dap::Scope* ThisScope = nullptr;
    for (const dap::Scope& Scope : ScopesResponse.scopes)
    {
        if (Scope.name == "This")
        {
            ThisScope = &Scope;
            break;
        }
    }
    if (ThisScope == nullptr)
    {
        State.Fail(TEXT("The paused frame had no This scope."));
        Finish();
        return;
    }

    dap::VariablesRequest VariablesRequest;
    VariablesRequest.variablesReference = ThisScope->variablesReference;
    dap::VariablesResponse VariablesResponse;
    if (!Send(*Session, VariablesRequest, VariablesResponse, State, Stop, TEXT("variables")))
    {
        Finish();
        return;
    }

    const dap::Variable* ArrayVariable = nullptr;
    const dap::Variable* ActorVariable = nullptr;
    bool bFoundVariable = false;
    for (const dap::Variable& Variable : VariablesResponse.variables)
    {
        if (Variable.name == TCHAR_TO_UTF8(VariableName))
        {
            bFoundVariable = true;
        }
        else if (Variable.name == "ArrayVar")
        {
            ArrayVariable = &Variable;
        }
        else if (Variable.name == "ActorVar")
        {
            ActorVariable = &Variable;
        }
    }
    if (!bFoundVariable)
    {
        State.Fail(FString::Printf(TEXT("The This scope did not expose %s."), VariableName));
        Finish();
        return;
    }

    dap::SetVariableRequest SetRequest;
    SetRequest.variablesReference = ThisScope->variablesReference;
    SetRequest.name = TCHAR_TO_UTF8(VariableName);
    SetRequest.value = TCHAR_TO_UTF8(ExpectedValueText);
    dap::SetVariableResponse SetResponse;
    if (Send(*Session, SetRequest, SetResponse, State, Stop, TEXT("setVariable")))
    {
        State.bSetVariable.store(true);
    }

    if (ArrayVariable != nullptr && ArrayVariable->variablesReference != 0)
    {
        dap::VariablesRequest ArrayRequest;
        ArrayRequest.variablesReference = ArrayVariable->variablesReference;
        dap::VariablesResponse ArrayResponse;
        if (Send(*Session, ArrayRequest, ArrayResponse, State, Stop, TEXT("array variables")))
        {
            bool bFoundElement = false;
            for (const dap::Variable& Element : ArrayResponse.variables)
            {
                if (Element.name == "[0]")
                {
                    bFoundElement = true;
                    break;
                }
            }

            if (bFoundElement)
            {
                dap::SetVariableRequest ElementRequest;
                ElementRequest.variablesReference = ArrayVariable->variablesReference;
                ElementRequest.name = "[0]";
                ElementRequest.value = TCHAR_TO_UTF8(ExpectedArrayValueText);
                dap::SetVariableResponse ElementResponse;
                if (Send(*Session, ElementRequest, ElementResponse, State, Stop, TEXT("set array element")))
                {
                    State.bArraySet.store(true);
                }
            }
            else
            {
                State.Fail(TEXT("ArrayVar had no [0] element."));
            }
        }
    }
    else
    {
        State.Fail(TEXT("ArrayVar was not exposed as a container."));
    }

    if (ActorVariable != nullptr)
    {
        dap::SetVariableRequest ActorRequest;
        ActorRequest.variablesReference = ThisScope->variablesReference;
        ActorRequest.name = "ActorVar";
        ActorRequest.value = "None";
        dap::SetVariableResponse ActorResponse;
        if (Send(*Session, ActorRequest, ActorResponse, State, Stop, TEXT("set object reference")))
        {
            State.bObjectSet.store(true);
        }
    }
    else
    {
        State.Fail(TEXT("ActorVar was not exposed in the This scope."));
    }

    // Resume even when setVariable failed, so the game thread cannot stay parked past the assertion.
    dap::ContinueRequest ContinueRequest;
    ContinueRequest.threadId = 1;
    dap::ContinueResponse ContinueResponse;
    ExitGuard.bResumed = Send(*Session, ContinueRequest, ContinueResponse, State, Stop, TEXT("continue"));
    Finish();
}

int64 FindFirstStatementAddress(const UFunction* Function)
{
    if (Function == nullptr)
    {
        return INDEX_NONE;
    }

    FBlueprintScriptDisassembler Disassembler;
    Disassembler.FunctionToNodes(Function);

    FBlueprintScriptVisualizer_Pseudo Visualizer;
    FString Text;
    FBlueprintSourceMap Map;
    Visualizer.Visualize(Function, Disassembler, Text, &Map);

    for (const FBlueprintLineInfo& Info : Map.Lines)
    {
        if (Info.Kind == EBlueprintLineKind::Statement)
        {
            return Info.ScriptIndex;
        }
    }
    return INDEX_NONE;
}
struct FBreakpointState : FClientState
{
    std::atomic<bool> bAuthenticated{false};
    std::atomic<bool> bBreakpointsSet{false};
    std::atomic<bool> bBreakpointVerified{false};
    std::atomic<bool> bStopped{false};
    std::atomic<bool> bResumed{false};
    // See FState::bDeferDone: the exit guard raises bDone after its cleanup requests have been answered.
    std::atomic<bool> bDeferDone{false};
    std::atomic<int32> StoppedLine{0};
    std::atomic<int32> ArmedLine{0};
};

enum class EBreakpointStage : uint8
{
    Idle,
    WaitingForBreakpoints,
    WaitingForDone,
    Done
};

// One client and one fixture actor per test run. The actor is weak because PIE owns it.
struct FBreakpointFixture
{
    TSharedPtr<FBreakpointState> State = MakeShared<FBreakpointState>();
    TWeakObjectPtr<AActor> Actor;
    TWeakObjectPtr<UFunction> Function;

    EBreakpointStage Stage = EBreakpointStage::Idle;
    double StageStart = -1.0;
    double PieWaitStart = -1.0;

    FWorker Worker;
};

void RunBreakpointClient(const FString& TokenPath, const FString& ClassName, FBreakpointFixture& Fixture)
{
    FBreakpointState& State = *Fixture.State;
    const std::atomic<bool>& Stop = Fixture.Worker.Stop();

    const auto Finish = [&State]() { if (!State.bDeferDone.load()) { State.bDone.store(true); } };

    std::shared_ptr<dap::ReaderWriter> ReaderWriter = Connect(Stop, State);
    if (!ReaderWriter)
    {
        Finish();
        return;
    }

    std::unique_ptr<dap::Session> Session = dap::Session::create();
    Session->registerHandler([&State](const dap::StoppedEvent&) { State.bStopped.store(true); });
    Session->bind(ReaderWriter, [&State]() {
        // ClosedHandler takes no message. The client closes its own session after the test;
        // a close before bDone means the transport/session failed mid-run.
        if (!State.bDone.load())
        {
            State.Fail(TEXT("The DAP session closed before the client finished."));
        }
    });

    FString Token;
    if (!ReadSessionToken(TokenPath, Stop, Token, State))
    {
        Finish();
        return;
    }

    dap::InitializeRequest InitRequest;
    InitRequest.adapterID = "blueprint-inspector";
    InitRequest.clientID = "BlueprintInspectorAutomation";
    InitRequest.linesStartAt1 = true;
    InitRequest.columnsStartAt1 = true;
    dap::InitializeResponse InitResponse;
    if (!Send(*Session, InitRequest, InitResponse, State, Stop, TEXT("initialize")))
    {
        Finish();
        return;
    }

    dap::LaunchRequest LaunchRequest;
    using RestartVariant = dap::variant<dap::array<dap::any>, dap::boolean, dap::integer, dap::null, dap::number, dap::object, dap::string>;
    dap::object Restart;
    Restart["token"] = dap::string(TCHAR_TO_UTF8(*Token));
    LaunchRequest.restart = RestartVariant(Restart);
    dap::LaunchResponse LaunchResponse;
    if (!Send(*Session, LaunchRequest, LaunchResponse, State, Stop, TEXT("launch")))
    {
        Finish();
        return;
    }
    State.bAuthenticated.store(true);

    dap::LoadedSourcesRequest LoadedRequest;
    dap::LoadedSourcesResponse LoadedResponse;
    if (!Send(*Session, LoadedRequest, LoadedResponse, State, Stop, TEXT("loadedSources")))
    {
        Finish();
        return;
    }

    const dap::Source* MainSource = nullptr;
    for (const dap::Source& Candidate : LoadedResponse.sources)
    {
        if (Candidate.name.has_value() && FString(UTF8_TO_TCHAR(Candidate.name.value().c_str())) == ClassName)
        {
            MainSource = &Candidate;
            break;
        }
    }
    if (MainSource == nullptr)
    {
        State.Fail(FString::Printf(TEXT("loadedSources has no source named '%s'."), *ClassName));
        Finish();
        return;
    }

    // Declared after MainSource: the guard's clear request points into LoadedResponse, so it has to be
    // destroyed before that response is. Any exit once the breakpoint is armed resumes and clears.
    FClientExitGuard<FBreakpointState> ExitGuard;
    ExitGuard.Session = Session.get();
    ExitGuard.State = &State;
    ExitGuard.Stop = &Stop;
    ExitGuard.Stopped = &State.bStopped;
    ExitGuard.BreakpointSource = MainSource;
    ExitGuard.DoneFlag = &State.bDone;
    State.bDeferDone.store(true);

    dap::SourceRequest SourceFetch;
    SourceFetch.source = *MainSource;
    SourceFetch.sourceReference = MainSource->sourceReference.value(0);
    dap::SourceResponse SourceFetched;
    if (!Send(*Session, SourceFetch, SourceFetched, State, Stop, TEXT("source")))
    {
        Finish();
        return;
    }

    TArray<FString> SourceLines;
    FString(UTF8_TO_TCHAR(SourceFetched.content.c_str())).ParseIntoArrayLines(SourceLines, false);

    const FString Header = FString::Printf(TEXT("%s("), FunctionName);
    int32 HeaderLine = INDEX_NONE;
    for (int32 Index = 0; Index < SourceLines.Num(); ++Index)
    {
        if (SourceLines[Index].Contains(Header))
        {
            HeaderLine = Index + 1;
            break;
        }
    }
    if (HeaderLine == INDEX_NONE)
    {
        State.Fail(FString::Printf(TEXT("The class source has no %s function."), FunctionName));
        Finish();
        return;
    }

    dap::BreakpointLocationsRequest LocationsRequest;
    LocationsRequest.source = *MainSource;
    LocationsRequest.line = HeaderLine;
    LocationsRequest.endLine = SourceLines.Num();
    dap::BreakpointLocationsResponse LocationsResponse;
    if (!Send(*Session, LocationsRequest, LocationsResponse, State, Stop, TEXT("breakpointLocations")))
    {
        Finish();
        return;
    }

    int32 ArmedLine = 0;
    for (const dap::BreakpointLocation& Location : LocationsResponse.breakpoints)
    {
        const int32 Line = static_cast<int32>(Location.line);
        // Pick the smallest breakpointable line after the header. The response order is not part of the
        // protocol, so taking the first match made the armed line depend on the adapter's ordering.
        if (Line > HeaderLine && (ArmedLine == 0 || Line < ArmedLine))
        {
            ArmedLine = Line;
        }
    }
    if (ArmedLine == 0)
    {
        State.Fail(TEXT("No breakpointable line follows the function header."));
        Finish();
        return;
    }

    dap::SourceBreakpoint Requested;
    Requested.line = ArmedLine;
    dap::SetBreakpointsRequest SetRequest;
    SetRequest.source = *MainSource;
    SetRequest.breakpoints = dap::array<dap::SourceBreakpoint>{Requested};
    // Arm the guard before the request: if setBreakpoints fails or times out the adapter may still have
    // created the breakpoint, and the exit path has to clear it for the tests that follow.
    ExitGuard.bBreakpointArmed = true;
    dap::SetBreakpointsResponse SetResponse;
    if (!Send(*Session, SetRequest, SetResponse, State, Stop, TEXT("setBreakpoints")))
    {
        Finish();
        return;
    }
    if (SetResponse.breakpoints.empty())
    {
        State.Fail(TEXT("setBreakpoints returned no breakpoint."));
        Finish();
        return;
    }
    State.bBreakpointVerified.store(SetResponse.breakpoints[0].verified);
    State.ArmedLine.store(static_cast<int32>(SetResponse.breakpoints[0].line.value(ArmedLine)));
    State.bBreakpointsSet.store(true);

    const double StopDeadline = FPlatformTime::Seconds() + 15.0;
    WaitWhileRunning(Stop, StopDeadline, [&State]() { return State.bStopped.load(); });
    if (!State.bStopped.load())
    {
        State.Fail(TEXT("The fixture never stopped at the DAP breakpoint."));
        Finish();
        return;
    }

    dap::StackTraceRequest StackRequest;
    StackRequest.threadId = 1;
    dap::StackTraceResponse StackResponse;
    if (!Send(*Session, StackRequest, StackResponse, State, Stop, TEXT("stackTrace")) || StackResponse.stackFrames.empty())
    {
        State.Fail(TEXT("The paused frame had no stack trace."));
        Finish();
        return;
    }

    const int32 StoppedLine = static_cast<int32>(StackResponse.stackFrames[0].line);
    State.StoppedLine.store(StoppedLine);

    dap::ContinueRequest ContinueRequest;
    ContinueRequest.threadId = 1;
    dap::ContinueResponse ContinueResponse;
    ExitGuard.bResumed = Send(*Session, ContinueRequest, ContinueResponse, State, Stop, TEXT("continue"));
    if (ExitGuard.bResumed)
    {
        State.bResumed.store(true);
    }

    // DAP replaces the source's whole set, so an empty request clears the breakpoint for later tests.
    dap::SetBreakpointsRequest ClearRequest;
    ClearRequest.source = *MainSource;
    ClearRequest.breakpoints = dap::array<dap::SourceBreakpoint>{};
    dap::SetBreakpointsResponse ClearResponse;
    ExitGuard.bCleared = Send(*Session, ClearRequest, ClearResponse, State, Stop, TEXT("clear breakpoints"));

    Finish();
}

} // namespace BlueprintDapSetVariableTest

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapSetVariableCallCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapSetVariableTest::FFixture>, Fixture);

bool FDapSetVariableCallCommand::Update()
{
    using namespace BlueprintDapSetVariableTest;

    FFixture& Run = *Fixture;
    FState& State = *Run.State;

    if (Run.bCallStarted)
    {
        return true;
    }
    if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
    {
        if (Run.PieWaitStart < 0.0)
        {
            Run.PieWaitStart = FPlatformTime::Seconds();
        }
        if (FPlatformTime::Seconds() - Run.PieWaitStart > 60.0)
        {
            Test->AddError(TEXT("PIE did not start; the DAP setVariable test could not run."));
            return true;
        }
        return false;
    }

    Run.bCallStarted = true;
    UClass* const Class = BlueprintFixtureSource::GetMainClass();
    UFunction* const Function = Class ? Class->FindFunctionByName(FName(FunctionName)) : nullptr;
    if (Class == nullptr || Function == nullptr)
    {
        Test->AddError(TEXT("flow_switch is missing from the main fixture class."));
        return true;
    }

    const int64 Address = FindFirstStatementAddress(Function);
    if (Address == INDEX_NONE)
    {
        Test->AddError(TEXT("flow_switch has no statement address to arm."));
        return true;
    }

    UGameInstance* const GameInstance = GEditor->PlayWorld->GetGameInstance();
    UBlueprintInspectorSubsystem* const Subsystem = GameInstance ? GameInstance->GetSubsystem<UBlueprintInspectorSubsystem>() : nullptr;
    if (Subsystem == nullptr || !Subsystem->TestEnsureBreakpoint(Function, Address))
    {
        Test->AddError(TEXT("Could not arm the breakpoint on the DAP subsystem."));
        return true;
    }

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    AActor* const Demo = GEditor->PlayWorld->SpawnActor<AActor>(Class, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
    if (Demo == nullptr)
    {
        Test->AddError(TEXT("Could not spawn the fixture actor in the PIE world."));
        Subsystem->TestClearBreakpoint(Function, Address);
        return true;
    }
    if (const FObjectPropertyBase* const ActorProperty = FindFProperty<FObjectPropertyBase>(Class, TEXT("ActorVar")))
    {
        ActorProperty->SetObjectPropertyValue_InContainer(Demo, Demo);
    }
    if (const FArrayProperty* const ArrayProperty = FindFProperty<FArrayProperty>(Class, TEXT("ArrayVar")))
    {
        FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Demo));
        Helper.Resize(1);
        if (const FIntProperty* const Inner = CastField<FIntProperty>(ArrayProperty->Inner))
        {
            Inner->SetPropertyValue(Helper.GetRawPtr(0), 0);
        }
    }

    const FString TokenPath = FUBlueprintInspectorDap::GetTokenFilePath();
    // A token left behind by an interrupted run would otherwise be read instead of this run's.
    IFileManager::Get().Delete(*TokenPath, false, true, true);

    FFixture* const RunPtr = &Run;
    if (!Run.Worker.Start(Run.State, Fixture, [RunPtr, TokenPath]() { RunClient(TokenPath, *RunPtr); }))
    {
        Test->AddError(TEXT("The DAP setVariable client thread could not start."));
        Subsystem->TestClearBreakpoint(Function, Address);
        return true;
    }

    const double AuthDeadline = FPlatformTime::Seconds() + 10.0;
    WaitWhileRunning(Run.Worker.Stop(), AuthDeadline, [&State]() { return State.bAuthenticated.load() || State.bDone.load(); });

    if (!State.bAuthenticated.load())
    {
        State.Fail(TEXT("The DAP client did not authenticate before the fixture call."));
    }
    else if (!State.bDone.load())
    {
        State.bCallInFlight.store(true);
        Demo->ProcessEvent(Function, nullptr);
    }

    if (!Run.Worker.WaitForExit(15.0))
    {
        // The game thread is the only thread that can serve the client's requests, so this command
        // stops the client rather than leaving it to drive the adapter through PIE teardown.
        State.Fail(TEXT("The DAP client did not finish in time."));
        if (!Run.Worker.StopAndJoin(10.0))
        {
            Test->AddError(TEXT("The DAP setVariable client did not stop after the timeout."));
            Subsystem->TestClearBreakpoint(Function, Address);
            return true;
        }
    }
    else
    {
        Run.Worker.JoinIfFinished();
    }

    if (const FIntProperty* const Property = FindFProperty<FIntProperty>(Class, TEXT("IntVar")))
    {
        State.Value.store(Property->GetPropertyValue_InContainer(Demo));
    }
    else
    {
        State.Fail(TEXT("The fixture class has no IntVar property."));
    }

    if (const FArrayProperty* const ArrayProperty = FindFProperty<FArrayProperty>(Class, TEXT("ArrayVar")))
    {
        FScriptArrayHelper Helper(ArrayProperty, ArrayProperty->ContainerPtrToValuePtr<void>(Demo));
        if (Helper.Num() > 0)
        {
            if (const FIntProperty* const Inner = CastField<FIntProperty>(ArrayProperty->Inner))
            {
                State.ArrayValue.store(Inner->GetPropertyValue(Helper.GetRawPtr(0)));
            }
        }
    }

    if (const FObjectPropertyBase* const ActorProperty = FindFProperty<FObjectPropertyBase>(Class, TEXT("ActorVar")))
    {
        State.bObjectCleared.store(ActorProperty->GetObjectPropertyValue_InContainer(Demo) == nullptr);
    }

    Subsystem->TestClearBreakpoint(Function, Address);
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapSetVariableAssertCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapSetVariableTest::FFixture>, Fixture);

bool FDapSetVariableAssertCommand::Update()
{
    using namespace BlueprintDapSetVariableTest;

    if (!Fixture->Worker.Shutdown())
    {
        Test->AddError(TEXT("The DAP setVariable client was still running when the test finished."));
        return true;
    }

    FState& State = *Fixture->State;
    const FString Error = State.GetError();

    Test->TestTrue(TEXT("the DAP client finished"), State.bDone.load());
    Test->TestTrue(TEXT("setVariable succeeded"), State.bSetVariable.load());
    Test->TestEqual(TEXT("the paused actor's IntVar changed"), State.Value.load(), ExpectedValue);
    Test->TestTrue(TEXT("the array element was set"), State.bArraySet.load());
    Test->TestEqual(TEXT("the paused actor's array element changed"), State.ArrayValue.load(), ExpectedArrayValue);
    Test->TestTrue(TEXT("the object reference was set"), State.bObjectSet.load());
    Test->TestTrue(TEXT("the object reference was cleared"), State.bObjectCleared.load());
    if (!Error.IsEmpty())
    {
        Test->AddError(Error);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDapSetVariablePausedTest, "BlueprintInspector.Debugger.DapSetVariablePaused", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDapSetVariablePausedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<BlueprintDapSetVariableTest::FFixture> Fixture = MakeShared<BlueprintDapSetVariableTest::FFixture>();

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(GetBlueprintTestMapPath()));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FDapSetVariableCallCommand(this, Fixture));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FDapSetVariableAssertCommand(this, Fixture));
    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapBreakpointCallCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapSetVariableTest::FBreakpointFixture>, Fixture);

bool FDapBreakpointCallCommand::Update()
{
    using namespace BlueprintDapSetVariableTest;

    FBreakpointFixture& Run = *Fixture;
    FBreakpointState& State = *Run.State;

    if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
    {
        if (Run.PieWaitStart < 0.0)
        {
            Run.PieWaitStart = FPlatformTime::Seconds();
        }
        if (FPlatformTime::Seconds() - Run.PieWaitStart > 60.0)
        {
            Test->AddError(TEXT("PIE did not start; the DAP breakpoint test could not run."));
            return true;
        }
        return false;
    }

    if (Run.Stage == EBreakpointStage::Idle)
    {
        UClass* const Class = BlueprintFixtureSource::GetMainClass();
        UFunction* const Function = Class ? Class->FindFunctionByName(FName(FunctionName)) : nullptr;
        if (Class == nullptr || Function == nullptr)
        {
            Test->AddError(TEXT("flow_switch is missing from the main fixture class."));
            Run.Stage = EBreakpointStage::Done;
            return true;
        }

        const FString ClassName = Class->GetFName().ToString();

        FActorSpawnParameters SpawnParameters;
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* const Demo = GEditor->PlayWorld->SpawnActor<AActor>(Class, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
        if (Demo == nullptr)
        {
            Test->AddError(TEXT("Could not spawn the fixture actor in the PIE world."));
            Run.Stage = EBreakpointStage::Done;
            return true;
        }

        Run.Actor = Demo;
        Run.Function = Function;

        const FString TokenPath = FUBlueprintInspectorDap::GetTokenFilePath();
        // A token left behind by an interrupted run would otherwise be read instead of this run's.
        IFileManager::Get().Delete(*TokenPath, false, true, true);
        if (IFileManager::Get().FileExists(*TokenPath))
        {
            Test->AddError(FString::Printf(TEXT("The stale DAP session token at %s could not be removed; the run would read it instead."), *TokenPath));
            Run.Stage = EBreakpointStage::Done;
            return true;
        }

        FBreakpointFixture* const RunPtr = &Run;
        if (!Run.Worker.Start(Run.State, Fixture, [RunPtr, TokenPath, ClassName]() { RunBreakpointClient(TokenPath, ClassName, *RunPtr); }))
        {
            Test->AddError(TEXT("The DAP breakpoint client thread could not start."));
            Run.Stage = EBreakpointStage::Done;
            return true;
        }

        Run.Stage = EBreakpointStage::WaitingForBreakpoints;
        Run.StageStart = FPlatformTime::Seconds();
        return false;
    }

    if (Run.Stage == EBreakpointStage::WaitingForBreakpoints)
    {
        if (!State.bBreakpointsSet.load())
        {
            if (State.bDone.load() || FPlatformTime::Seconds() - Run.StageStart > 20.0)
            {
                if (!State.bDone.load())
                {
                    State.Fail(TEXT("The DAP client did not arm the breakpoint in time."));
                    if (!Run.Worker.StopAndJoin(10.0))
                    {
                        Test->AddError(TEXT("The DAP breakpoint client did not stop after the timeout."));
                        Run.Stage = EBreakpointStage::Done;
                        return true;
                    }
                }
                else
                {
                    Run.Worker.JoinIfFinished();
                }
                Run.Stage = EBreakpointStage::Done;
                return true;
            }
            // Yield so the game thread can serve the gated setBreakpoints request queued above.
            return false;
        }

        if (!Run.Actor.IsValid() || !Run.Function.IsValid())
        {
            Test->AddError(TEXT("The fixture actor was gone before the breakpoint call."));
            if (!Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(TEXT("The DAP breakpoint client did not stop after the fixture actor was lost."));
            }
            Run.Stage = EBreakpointStage::Done;
            return true;
        }
        if (State.bDone.load())
        {
            Run.Worker.JoinIfFinished();
            Run.Stage = EBreakpointStage::Done;
            return true;
        }
        Run.Actor->ProcessEvent(Run.Function.Get(), nullptr);
        Run.Stage = EBreakpointStage::WaitingForDone;
        Run.StageStart = FPlatformTime::Seconds();
        return false;
    }

    if (Run.Stage == EBreakpointStage::WaitingForDone)
    {
        if (State.bDone.load())
        {
            Run.Worker.JoinIfFinished();
            Run.Stage = EBreakpointStage::Done;
            return true;
        }

        if (FPlatformTime::Seconds() - Run.StageStart > 20.0)
        {
            State.Fail(TEXT("The DAP breakpoint client did not finish."));
            if (!Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(TEXT("The DAP breakpoint client did not stop after the timeout."));
                Run.Stage = EBreakpointStage::Done;
                return true;
            }
            Run.Stage = EBreakpointStage::Done;
            return true;
        }
        return false;
    }

    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapBreakpointAssertCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapSetVariableTest::FBreakpointFixture>, Fixture);

bool FDapBreakpointAssertCommand::Update()
{
    using namespace BlueprintDapSetVariableTest;

    if (!Fixture->Worker.Shutdown())
    {
        Test->AddError(TEXT("The DAP breakpoint client was still running when the test finished."));
        return true;
    }

    FBreakpointState& State = *Fixture->State;
    const FString Error = State.GetError();

    Test->TestTrue(TEXT("the DAP breakpoint client finished"), State.bDone.load());
    Test->TestTrue(TEXT("the DAP client authenticated"), State.bAuthenticated.load());
    Test->TestTrue(TEXT("setBreakpoints verified the breakpoint"), State.bBreakpointVerified.load());
    Test->TestTrue(TEXT("setBreakpoints armed a line"), State.ArmedLine.load() > 0);
    Test->TestTrue(TEXT("the fixture stopped at the breakpoint"), State.bStopped.load());
    Test->TestEqual(TEXT("the stopped frame is on the armed line"), State.StoppedLine.load(), State.ArmedLine.load());
    Test->TestTrue(TEXT("the client resumed after the stop"), State.bResumed.load());
    if (!Error.IsEmpty())
    {
        Test->AddError(Error);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDapBreakpointPausedTest, "BlueprintInspector.Debugger.DapBreakpointPaused", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDapBreakpointPausedTest::RunTest(const FString& Parameters)
{
    TSharedPtr<BlueprintDapSetVariableTest::FBreakpointFixture> Fixture = MakeShared<BlueprintDapSetVariableTest::FBreakpointFixture>();

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(GetBlueprintTestMapPath()));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FDapBreakpointCallCommand(this, Fixture));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FDapBreakpointAssertCommand(this, Fixture));
    return true;
}

#endif
