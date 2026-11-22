// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

#include "Editor.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "BlueprintDapTestClient.h"
#include "BlueprintFixtureSource.h"
#include "BlueprintInspectorDAP.h"
#include "BlueprintInspectorPort.h"
#include "BlueprintTestConfiguration.h"

#include "dap/network.h"
#include "dap/session.h"

#include <atomic>
#include <memory>

namespace BlueprintDapExceptionStopTest
{
using namespace BlueprintDapTest;

// The fixture reads ArrayVar[1] and the transient fixture leaves ArrayVar empty, so calling it raises
// the VM's out-of-bounds access violation. No new fixture asset is needed for a runtime error.
const TCHAR* const FunctionName = TEXT("flow_arraygetbyref");

struct FState : FClientState
{
    std::atomic<bool> bAuthenticated{false};
    std::atomic<bool> bRendered{false};
    std::atomic<bool> bFiltersAdvertised{false};
    std::atomic<bool> bArmed{false};
    std::atomic<bool> bFirstCallReturned{false};
    std::atomic<bool> bContinued{false};
    // Set by every exception stop and cleared only after a continue has been served. bContinued cannot
    // do this job: it stays true after the first resume, so a later stop would never be released.
    std::atomic<bool> bResumePending{false};
    std::atomic<bool> bCleared{false};
    std::atomic<bool> bRunAgain{false};
    std::atomic<bool> bSecondCallDone{false};
    std::atomic<bool> bParkedUnreleased{false};

    std::atomic<int32> StoppedCount{0};
    std::atomic<int32> StoppedThreadId{1};
    std::atomic<bool> bExceptionReason{false};
    std::atomic<bool> bExceptionText{false};
    std::atomic<bool> bExceptionDescription{false};
    std::atomic<int32> StackFrames{0};
    std::atomic<int32> TopLine{0};
    std::atomic<int32> TopFrameFunction{0};
};

enum class EStage
{
    WaitForPie,
    WaitingForArm,
    WaitingForSecondCall,
    WaitingForDone,
    Done
};

// One client and one fixture actor per test run. The actor is weak because PIE owns it.
struct FFixture
{
    TSharedPtr<FState> State = MakeShared<FState>();
    TWeakObjectPtr<AActor> Actor;
    TWeakObjectPtr<UFunction> Function;
    FString ClassName;

    EStage Stage = EStage::WaitForPie;
    double StageStart = -1.0;
    double PieWaitStart = -1.0;

    FWorker Worker;
};

void RunClient(const FString& TokenPath, const FString& ClassName, FFixture& Fixture)
{
    FState& State = *Fixture.State;
    const std::atomic<bool>& Stop = Fixture.Worker.Stop();

    std::shared_ptr<dap::ReaderWriter> ReaderWriter = Connect(Stop, State);
    if (!ReaderWriter)
    {
        return;
    }

    std::unique_ptr<dap::Session> Session = dap::Session::create();
    Session->registerHandler([&State](const dap::StoppedEvent& Event) {
        State.bResumePending.store(true);
        if (Event.threadId.has_value())
        {
            State.StoppedThreadId.store(static_cast<int32>(Event.threadId.value()));
        }
        if (Event.reason != "exception")
        {
            return;
        }
        State.StoppedCount.fetch_add(1);
        State.bExceptionReason.store(true);
        if (Event.text.has_value() && Event.text.value().find("Attempted to access index") != std::string::npos)
        {
            State.bExceptionText.store(true);
        }
        if (Event.description.has_value() && !Event.description.value().empty())
        {
            State.bExceptionDescription.store(true);
        }
    });
    Session->bind(ReaderWriter, [&State]() {
        // ClosedHandler takes no message. The client closes its own session after the test;
        // a close before bDone means the transport/session failed mid-run.
        if (!State.bDone.load())
        {
            State.Fail(TEXT("The DAP session closed before the client finished."));
        }
    });

    // Declared after the session so it is destroyed first: bDone must be raised before the socket
    // closes, or the closed handler would report a failure on the normal path.
    FDoneGuard Done{State.bDone};

    FString Token;
    if (!ReadSessionToken(TokenPath, Stop, Token, State))
    {
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
        return;
    }

    // The filters the client offers are what a real exception UI lists; a missing one would leave the
    // stop unreachable from the editor.
    if (InitResponse.exceptionBreakpointFilters.has_value())
    {
        bool bAccessedNone = false;
        bool bRuntimeError = false;
        bool bInfiniteLoop = false;
        for (const dap::ExceptionBreakpointsFilter& Filter : InitResponse.exceptionBreakpointFilters.value())
        {
            bAccessedNone = bAccessedNone || Filter.filter == "accessedNone";
            bRuntimeError = bRuntimeError || Filter.filter == "runtimeError";
            bInfiniteLoop = bInfiniteLoop || Filter.filter == "infiniteLoop";
        }
        State.bFiltersAdvertised.store(bAccessedNone && bRuntimeError && bInfiniteLoop);
    }
    if (!State.bFiltersAdvertised.load())
    {
        State.Fail(TEXT("initialize did not advertise the accessedNone, runtimeError and infiniteLoop filters."));
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
        return;
    }
    State.bAuthenticated.store(true);

    // Render the class before the error: the stop reports a line, and that needs a cached source map.
    dap::LoadedSourcesRequest LoadedRequest;
    dap::LoadedSourcesResponse LoadedResponse;
    if (!Send(*Session, LoadedRequest, LoadedResponse, State, Stop, TEXT("loadedSources")))
    {
        return;
    }

    const dap::Source* FixtureSource = nullptr;
    for (const dap::Source& Candidate : LoadedResponse.sources)
    {
        if (Candidate.name.has_value() && FString(UTF8_TO_TCHAR(Candidate.name.value().c_str())) == ClassName)
        {
            FixtureSource = &Candidate;
            break;
        }
    }
    if (FixtureSource == nullptr)
    {
        State.Fail(FString::Printf(TEXT("loadedSources has no source named '%s'."), *ClassName));
        return;
    }

    dap::SourceRequest SourceFetch;
    SourceFetch.source = *FixtureSource;
    SourceFetch.sourceReference = FixtureSource->sourceReference.value(0);
    dap::SourceResponse SourceFetched;
    if (!Send(*Session, SourceFetch, SourceFetched, State, Stop, TEXT("source")))
    {
        return;
    }
    State.bRendered.store(true);

    // Arm the filter. From here an access violation parks the game thread.
    dap::SetExceptionBreakpointsRequest ArmRequest;
    ArmRequest.filters.push_back("accessedNone");
    dap::SetExceptionBreakpointsResponse ArmResponse;
    if (!Send(*Session, ArmRequest, ArmResponse, State, Stop, TEXT("setExceptionBreakpoints")))
    {
        return;
    }
    State.bArmed.store(true);

    // From here the VM parks if the fixture raises. Any early return or exception in this function must
    // release it, or the latent command never reaches the assertion stage.
    struct FResumeGuard
    {
        dap::Session* Session = nullptr;
        FState* State = nullptr;
        const std::atomic<bool>* Stop = nullptr;

        ~FResumeGuard()
        {
            if (Session == nullptr || State == nullptr || Stop == nullptr)
            {
                return;
            }
            if (!State->bResumePending.load() && !(State->bArmed.load() && !State->bFirstCallReturned.load()))
            {
                return;
            }

            for (int32 Attempt = 0; Attempt < 2; ++Attempt)
            {
                dap::ContinueRequest ContinueRequest;
                ContinueRequest.threadId = State->StoppedThreadId.load();
                dap::ContinueResponse ContinueResponse;
                try
                {
                    // Ignore the stop flag: the game thread is parked until this request is served.
                    if (Send(*Session, ContinueRequest, ContinueResponse, *State, *Stop, TEXT("continue (failure cleanup)"), 5.0, true))
                    {
                        State->bResumePending.store(false);
                        return;
                    }
                }
                catch (const std::exception& Exception)
                {
                    State->Fail(FString::Printf(TEXT("The cleanup continue threw: %s"), UTF8_TO_TCHAR(Exception.what())));
                }
                // NOLINTNEXTLINE(bugprone-empty-catch): a destructor must not let cppdap's exceptions escape.
                catch (...)
                {
                    State->Fail(TEXT("The cleanup continue threw an unknown exception."));
                }
            }

            State->bParkedUnreleased.store(true);
        }
    };
    FResumeGuard Resume{Session.get(), &State, &Stop};

    const double StopDeadline = FPlatformTime::Seconds() + 20.0;
    WaitWhileRunning(Stop, StopDeadline, [&State]() { return State.StoppedCount.load() > 0; });
    if (State.StoppedCount.load() == 0)
    {
        State.Fail(TEXT("The runtime error did not stop execution with the accessedNone filter on."));
        return;
    }

    dap::StackTraceRequest StackRequest;
    StackRequest.threadId = State.StoppedThreadId.load();
    dap::StackTraceResponse StackResponse;
    if (!Send(*Session, StackRequest, StackResponse, State, Stop, TEXT("stackTrace")))
    {
        return;
    }
    State.StackFrames.store(static_cast<int32>(StackResponse.stackFrames.size()));
    if (!StackResponse.stackFrames.empty())
    {
        const dap::StackFrame& Top = StackResponse.stackFrames[0];
        State.TopLine.store(static_cast<int32>(Top.line));
        State.TopFrameFunction.store(FString(UTF8_TO_TCHAR(Top.name.c_str())) == FString(FunctionName) ? 1 : 0);
    }

    dap::ContinueRequest ContinueRequest;
    ContinueRequest.threadId = State.StoppedThreadId.load();
    dap::ContinueResponse ContinueResponse;
    const int32 StopsBeforeContinue = State.StoppedCount.load();
    if (!Send(*Session, ContinueRequest, ContinueResponse, State, Stop, TEXT("continue")))
    {
        return;
    }
    State.bContinued.store(true);
    if (State.StoppedCount.load() == StopsBeforeContinue)
    {
        State.bResumePending.store(false);
    }

    // With the filters cleared the same error must run past: only the log reports it.
    dap::SetExceptionBreakpointsRequest ClearRequest;
    dap::SetExceptionBreakpointsResponse ClearResponse;
    if (!Send(*Session, ClearRequest, ClearResponse, State, Stop, TEXT("setExceptionBreakpoints (clear)")))
    {
        return;
    }
    State.bCleared.store(true);
    State.bRunAgain.store(true);

    const double SecondDeadline = FPlatformTime::Seconds() + 20.0;
    WaitWhileRunning(Stop, SecondDeadline, [&State]() { return State.bSecondCallDone.load(); });
    if (!State.bSecondCallDone.load())
    {
        State.Fail(TEXT("The fixture did not run the failing function a second time."));
        return;
    }

    // A late stop would arrive right after the call returns; give it a moment to show up before the
    // assertion reads the count.
    FPlatformProcess::Sleep(0.5f);
}

void ExpectRaiseError(FAutomationTestBase& Test)
{
#if BI_ENGINE_AT_LEAST_5
    Test.AddExpectedError(TEXT("Blueprint Runtime Error"), EAutomationExpectedErrorFlags::Contains, 0);
#else
    Test.AddExpectedError(TEXT("Attempted to access index"), EAutomationExpectedErrorFlags::Contains, 0);
#endif
}

} // namespace BlueprintDapExceptionStopTest

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapExceptionCallCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapExceptionStopTest::FFixture>, Fixture);

bool FDapExceptionCallCommand::Update()
{
    using namespace BlueprintDapExceptionStopTest;

    FFixture& Run = *Fixture;

    if (Run.Stage == EStage::WaitForPie)
    {
        if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
        {
            if (Run.PieWaitStart < 0.0)
            {
                Run.PieWaitStart = FPlatformTime::Seconds();
            }
            if (FPlatformTime::Seconds() - Run.PieWaitStart > 60.0)
            {
                Test->AddError(TEXT("PIE did not start; the DAP exception-stop test could not run."));
                Run.Stage = EStage::Done;
                return true;
            }
            return false;
        }

        UClass* const Class = BlueprintFixtureSource::GetMainClass();
        UFunction* const Function = Class ? Class->FindFunctionByName(FName(FunctionName)) : nullptr;
        if (Class == nullptr || Function == nullptr)
        {
            Test->AddError(FString::Printf(TEXT("%s is missing from the main fixture class."), FunctionName));
            Run.Stage = EStage::Done;
            return true;
        }

        FActorSpawnParameters SpawnParameters;
        SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        AActor* const Actor = GEditor->PlayWorld->SpawnActor<AActor>(Class, FVector::ZeroVector, FRotator::ZeroRotator, SpawnParameters);
        if (Actor == nullptr)
        {
            Test->AddError(TEXT("Could not spawn the fixture actor in the PIE world."));
            Run.Stage = EStage::Done;
            return true;
        }

        Run.Actor = Actor;
        Run.Function = Function;
        Run.ClassName = Class->GetFName().ToString();

        const FString TokenPath = FUBlueprintInspectorDap::GetTokenFilePath();
        // A token left behind by an interrupted run would otherwise be read instead of this run's.
        IFileManager::Get().Delete(*TokenPath, false, true, true);
        if (IFileManager::Get().FileExists(*TokenPath))
        {
            Test->AddError(FString::Printf(TEXT("The stale DAP session token at %s could not be removed; the run would read it instead."), *TokenPath));
            Run.Stage = EStage::Done;
            return true;
        }

        FFixture* const RunPtr = &Run;
        const FString ClassName = Run.ClassName;
        if (!Run.Worker.Start(Run.State, Fixture, [RunPtr, TokenPath, ClassName]() { RunClient(TokenPath, ClassName, *RunPtr); }))
        {
            Test->AddError(TEXT("The DAP exception-stop client thread could not start."));
            Run.Stage = EStage::Done;
            return true;
        }

        Run.Stage = EStage::WaitingForArm;
        Run.StageStart = FPlatformTime::Seconds();
        return false;
    }

    if (Run.Stage == EStage::WaitingForArm)
    {
        if (Run.State->bArmed.load())
        {
            if (!Run.Actor.IsValid() || !Run.Function.IsValid())
            {
                Test->AddError(TEXT("The fixture actor was gone before the failing call."));
                if (!Run.Worker.StopAndJoin(10.0))
                {
                    Test->AddError(Run.State->bParkedUnreleased.load()
                                       ? TEXT("The DAP exception client left the game thread parked.")
                                       : TEXT("The DAP exception client did not stop after the fixture actor was lost."));
                }
                Run.Stage = EStage::Done;
                return true;
            }
            // The exception parks the game thread inside this call until the client continues.
            ExpectRaiseError(*Test);
            Run.Actor->ProcessEvent(Run.Function.Get(), nullptr);
            Run.State->bFirstCallReturned.store(true);
            Run.Stage = EStage::WaitingForSecondCall;
            Run.StageStart = FPlatformTime::Seconds();
            return false;
        }

        if (FPlatformTime::Seconds() - Run.StageStart > 30.0)
        {
            Run.State->Fail(TEXT("The DAP client did not arm the exception filter in time."));
            if (!Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(Run.State->bParkedUnreleased.load()
                                   ? TEXT("The DAP exception client left the game thread parked.")
                                   : TEXT("The DAP exception client did not stop after the timeout."));
            }
            Run.Stage = EStage::Done;
            return true;
        }
        return false;
    }

    if (Run.Stage == EStage::WaitingForSecondCall)
    {
        if (Run.State->bRunAgain.load())
        {
            if (!Run.Actor.IsValid() || !Run.Function.IsValid())
            {
                Test->AddError(TEXT("The fixture actor was gone before the second call."));
                if (!Run.Worker.StopAndJoin(10.0))
                {
                    Test->AddError(TEXT("The DAP exception client did not stop after the fixture actor was lost."));
                }
                Run.Stage = EStage::Done;
                return true;
            }
            Run.Actor->ProcessEvent(Run.Function.Get(), nullptr);
            Run.State->bSecondCallDone.store(true);
            Run.Stage = EStage::WaitingForDone;
            Run.StageStart = FPlatformTime::Seconds();
            return false;
        }

        if (FPlatformTime::Seconds() - Run.StageStart > 30.0)
        {
            Run.State->Fail(TEXT("The DAP client did not clear the exception filter in time."));
            if (!Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(Run.State->bParkedUnreleased.load()
                                   ? TEXT("The DAP exception client left the game thread parked.")
                                   : TEXT("The DAP exception client did not stop after the timeout."));
            }
            Run.Stage = EStage::Done;
            return true;
        }
        return false;
    }

    if (Run.Stage == EStage::WaitingForDone)
    {
        if (Run.State->bDone.load())
        {
            Run.Worker.JoinIfFinished();
            Run.Stage = EStage::Done;
            return true;
        }

        if (FPlatformTime::Seconds() - Run.StageStart > 30.0)
        {
            Run.State->Fail(TEXT("The DAP exception client did not finish."));
            if (!Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(Run.State->bParkedUnreleased.load()
                                   ? TEXT("The DAP exception client left the game thread parked.")
                                   : TEXT("The DAP exception client did not stop after the timeout."));
            }
            Run.Stage = EStage::Done;
            return true;
        }
        return false;
    }

    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapExceptionAssertCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapExceptionStopTest::FFixture>, Fixture);

bool FDapExceptionAssertCommand::Update()
{
    using namespace BlueprintDapExceptionStopTest;

    FState& State = *Fixture->State;

    if (!Fixture->Worker.Shutdown())
    {
        Test->AddError(TEXT("The DAP exception client was still running when the test finished."));
    }

    const FString Error = State.GetError();

    Test->TestTrue(TEXT("the DAP client finished"), State.bDone.load());
    Test->TestTrue(TEXT("the DAP client authenticated"), State.bAuthenticated.load());
    Test->TestTrue(TEXT("initialize advertised the exception filters"), State.bFiltersAdvertised.load());
    Test->TestTrue(TEXT("the fixture source was rendered"), State.bRendered.load());
    Test->TestTrue(TEXT("setExceptionBreakpoints armed the filter"), State.bArmed.load());
    Test->TestEqual(TEXT("the runtime error stopped exactly once"), State.StoppedCount.load(), 1);
    Test->TestTrue(TEXT("the stop reason is exception"), State.bExceptionReason.load());
    Test->TestTrue(TEXT("the stop carries the VM's error text"), State.bExceptionText.load());
    Test->TestTrue(TEXT("the stop carries a description"), State.bExceptionDescription.load());
    Test->TestTrue(TEXT("the stop has a stack trace"), State.StackFrames.load() > 0);
    Test->TestTrue(TEXT("the failing line resolves to the fixture's array get"), State.TopLine.load() > 0);
    Test->TestTrue(TEXT("the top frame is the failing fixture function"), State.TopFrameFunction.load() == 1);
    Test->TestTrue(TEXT("the game thread resumed after the stop"), State.bFirstCallReturned.load());
    Test->TestTrue(TEXT("continue was served"), State.bContinued.load());
    Test->TestTrue(TEXT("setExceptionBreakpoints cleared the filter"), State.bCleared.load());
    Test->TestTrue(TEXT("the second call ran past the cleared filter"), State.bSecondCallDone.load());
    Test->TestFalse(TEXT("the parked game thread was released"), State.bParkedUnreleased.load());
    if (!Error.IsEmpty())
    {
        Test->AddError(Error);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDapExceptionStopTest, "BlueprintInspector.Debugger.DapExceptionStop", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDapExceptionStopTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintDapExceptionStopTest;

    TSharedPtr<BlueprintDapExceptionStopTest::FFixture> Fixture = MakeShared<BlueprintDapExceptionStopTest::FFixture>();
    Fixture->Stage = BlueprintDapExceptionStopTest::EStage::WaitForPie;

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(GetBlueprintTestMapPath()));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FDapExceptionCallCommand(this, Fixture));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FDapExceptionAssertCommand(this, Fixture));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
