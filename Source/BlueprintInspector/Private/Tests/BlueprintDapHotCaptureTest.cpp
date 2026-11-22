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
#include "BlueprintTestConfiguration.h"

#include "dap/network.h"
#include "dap/session.h"

#include <atomic>
#include <memory>

namespace BlueprintDapHotCaptureTest
{
using namespace BlueprintDapTest;

const TCHAR* const FunctionName = TEXT("flow_switch");
constexpr int32 CallsUnderCapture = 3;
constexpr int32 MaxLines = 2000;
constexpr int32 CoverageMaxLines = 50000;

struct FState : FClientState
{
    std::atomic<bool> bAuthenticated{false};
    std::atomic<bool> bRendered{false};
    std::atomic<bool> bCaptureStarted{false};
    std::atomic<bool> bFunctionCalled{false};
    std::atomic<bool> bStartCleared{false};
    std::atomic<bool> bStopKeptCounters{false};
    std::atomic<bool> bResetCleared{false};
    std::atomic<bool> bCoverageTotals{false};
    std::atomic<bool> bCoverageKinds{false};
    std::atomic<bool> bCoverageResetCleared{false};
    std::atomic<bool> bMetadata{false};
    std::atomic<int64> RawBeforeStop{0};
    std::atomic<int32> MatchingLines{0};
    std::atomic<int32> CoverageCovered{0};
    std::atomic<int32> CoverageUncovered{0};
};

enum class EStage
{
    WaitForPie,
    WaitingForCall,
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
        // Surface the connection failure now instead of letting the command wait out its stage timeout.
        State.bDone.store(true);
        return;
    }

    std::unique_ptr<dap::Session> Session = dap::Session::create();
    Session->bind(ReaderWriter, [&State]() {
        // The client closes its own session after the test; a close before bDone means the
        // transport/session failed mid-run.
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

    // Render the class before capturing: hotLines only resolves against a cached source map.
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

    dap::BlueprintStartCaptureRequest StartRequest;
    dap::BlueprintCaptureState StartState;
    if (!Send(*Session, StartRequest, StartState, State, Stop, TEXT("startCapture")))
    {
        return;
    }
    if (!StartState.capturing)
    {
        State.Fail(TEXT("startCapture did not enter capture mode."));
        return;
    }
    // Start resets the counters, so the response must already report zero.
    State.bStartCleared.store(StartState.totalHits == 0);
    State.bCaptureStarted.store(true);

    const double FunctionDeadline = FPlatformTime::Seconds() + 15.0;
    WaitWhileRunning(Stop, FunctionDeadline, [&State]() { return State.bFunctionCalled.load(); }, 0.02f);
    if (!State.bFunctionCalled.load())
    {
        State.Fail(TEXT("The game thread did not run the fixture function under capture."));
        return;
    }

    dap::BlueprintCaptureStateRequest StateRequest;
    dap::BlueprintCaptureState Captured;
    if (!Send(*Session, StateRequest, Captured, State, Stop, TEXT("captureState")))
    {
        return;
    }
    State.RawBeforeStop.store(Captured.totalHits);
    if (Captured.totalHits <= 0)
    {
        State.Fail(TEXT("capture counted no dispatches."));
        return;
    }

    const std::string ClassNameUtf8(TCHAR_TO_UTF8(*ClassName));
    const std::string FunctionNameUtf8(TCHAR_TO_UTF8(FunctionName));
    int32 Matching = 0;
    const double HotDeadline = FPlatformTime::Seconds() + 10.0;
    do
    {
        dap::BlueprintHotLinesRequest HotRequest;
        HotRequest.maxLines = MaxLines;
        dap::BlueprintHotLinesResponse HotResponse;
        if (!Send(*Session, HotRequest, HotResponse, State, Stop, TEXT("hotLines")))
        {
            return;
        }

        Matching = 0;
        for (const dap::BlueprintHotLine& Line : HotResponse.lines)
        {
            if (Line.className == ClassNameUtf8 && Line.functionName == FunctionNameUtf8 && Line.hits > 0)
            {
                ++Matching;
            }
        }
        if (Matching > 0)
        {
            break;
        }
        FPlatformProcess::Sleep(0.1f);
    } while (FPlatformTime::Seconds() < HotDeadline);

    State.MatchingLines.store(Matching);
    if (Matching == 0)
    {
        State.Fail(TEXT("hotLines returned no executed flow_switch line for the fixture class."));
        return;
    }

    // A snapshot has to say which run it describes, so the metadata names the map being played and
    // carries a build, engine, project and start time.
    dap::BlueprintHotLinesRequest MetadataRequest;
    MetadataRequest.maxLines = 1;
    dap::BlueprintHotLinesResponse MetadataResponse;
    if (!Send(*Session, MetadataRequest, MetadataResponse, State, Stop, TEXT("hotLines metadata")))
    {
        return;
    }
    const FString ExpectedMap = FPaths::GetBaseFilename(GetBlueprintTestMapPath());
    const std::string ExpectedMapUtf8(TCHAR_TO_UTF8(*ExpectedMap));
    State.bMetadata.store(MetadataResponse.metadata.map == ExpectedMapUtf8 && !MetadataResponse.metadata.build.empty() && !MetadataResponse.metadata.engine.empty() && !MetadataResponse.metadata.project.empty() && !MetadataResponse.metadata.capturedAt.empty());

    // Coverage must agree with the capture: the statements this run executed are covered and the
    // fixture's other functions are not, so both sides of the view are exercised by one run.
    dap::BlueprintCoverageRequest CoverageRequest;
    CoverageRequest.maxLines = CoverageMaxLines;
    dap::BlueprintCoverageResponse Coverage;
    if (!Send(*Session, CoverageRequest, Coverage, State, Stop, TEXT("coverage")))
    {
        return;
    }

    int32 CoverageCovered = 0;
    int32 CoverageUncovered = 0;
    bool bKindsValid = true;
    for (const dap::BlueprintCoverageLine& Line : Coverage.lines)
    {
        if (Line.className != ClassNameUtf8)
        {
            continue;
        }
        if (Line.kind != "statement" && Line.kind != "transfer")
        {
            bKindsValid = false;
        }
        if (Line.functionName.empty())
        {
            bKindsValid = false;
        }
        if (Line.hits > 0)
        {
            ++CoverageCovered;
        }
        else
        {
            ++CoverageUncovered;
        }
    }
    State.CoverageCovered.store(CoverageCovered);
    State.CoverageUncovered.store(CoverageUncovered);
    State.bCoverageKinds.store(bKindsValid);
    State.bCoverageTotals.store(Coverage.renderedClasses > 0 && !Coverage.truncated && static_cast<size_t>(Coverage.totalLines) == Coverage.lines.size() && Coverage.coveredLines + Coverage.uncoveredLines == Coverage.totalLines && Coverage.coveredLines > 0 && Coverage.uncoveredLines > 0);

    dap::BlueprintStopCaptureRequest StopRequest;
    dap::BlueprintCaptureState Stopped;
    if (!Send(*Session, StopRequest, Stopped, State, Stop, TEXT("stopCapture")))
    {
        return;
    }
    State.bStopKeptCounters.store(!Stopped.capturing && Stopped.totalHits > 0 && Stopped.totalHits >= Captured.totalHits);

    dap::BlueprintResetCaptureRequest ResetRequest;
    dap::BlueprintCaptureState ResetState;
    if (!Send(*Session, ResetRequest, ResetState, State, Stop, TEXT("resetCapture")))
    {
        return;
    }

    dap::BlueprintHotLinesRequest AfterRequest;
    AfterRequest.maxLines = MaxLines;
    dap::BlueprintHotLinesResponse AfterResponse;
    if (!Send(*Session, AfterRequest, AfterResponse, State, Stop, TEXT("hotLines after reset")))
    {
        return;
    }
    State.bResetCleared.store(ResetState.totalHits == 0 && !ResetState.capturing && AfterResponse.lines.empty() && AfterResponse.state.totalHits == 0);

    dap::BlueprintCoverageRequest CoverageAfterRequest;
    CoverageAfterRequest.maxLines = CoverageMaxLines;
    dap::BlueprintCoverageResponse CoverageAfter;
    if (!Send(*Session, CoverageAfterRequest, CoverageAfter, State, Stop, TEXT("coverage after reset")))
    {
        return;
    }
    // Reset clears the counters, not the rendered maps: the same lines come back, all uncovered.
    State.bCoverageResetCleared.store(CoverageAfter.totalLines > 0 && CoverageAfter.coveredLines == 0 && CoverageAfter.uncoveredLines == CoverageAfter.totalLines);
}

} // namespace BlueprintDapHotCaptureTest

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapHotCaptureCallCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapHotCaptureTest::FFixture>, Fixture);

bool FDapHotCaptureCallCommand::Update()
{
    using namespace BlueprintDapHotCaptureTest;

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
                Test->AddError(TEXT("PIE did not start; the DAP capture test could not run."));
                Run.Stage = EStage::Done;
                return true;
            }
            return false;
        }

        UClass* const Class = BlueprintFixtureSource::GetMainClass();
        UFunction* const Function = Class ? Class->FindFunctionByName(FName(FunctionName)) : nullptr;
        if (Class == nullptr || Function == nullptr)
        {
            Test->AddError(TEXT("flow_switch is missing from the main fixture class."));
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
            Test->AddError(TEXT("The DAP capture client thread could not start."));
            Run.Stage = EStage::Done;
            return true;
        }

        Run.Stage = EStage::WaitingForCall;
        Run.StageStart = FPlatformTime::Seconds();
        return false;
    }

    if (Run.Stage == EStage::WaitingForCall)
    {
        if (Run.State->bDone.load())
        {
            if (!Run.Worker.JoinIfFinished() && !Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(TEXT("The DAP capture client was still running when PIE teardown began."));
            }
            Run.Stage = EStage::Done;
            return true;
        }

        if (Run.State->bCaptureStarted.load())
        {
            if (!Run.Actor.IsValid() || !Run.Function.IsValid())
            {
                Test->AddError(TEXT("The fixture actor was gone before the capture calls."));
                if (!Run.Worker.StopAndJoin(10.0))
                {
                    Test->AddError(TEXT("The DAP capture client did not stop after the fixture actor was lost."));
                }
                Run.Stage = EStage::Done;
                return true;
            }
            // Capture is live; these calls are the statements hotLines must report.
            for (int32 Index = 0; Index < CallsUnderCapture; ++Index)
            {
                Run.Actor->ProcessEvent(Run.Function.Get(), nullptr);
            }
            Run.State->bFunctionCalled.store(true);
            Run.Stage = EStage::WaitingForDone;
            Run.StageStart = FPlatformTime::Seconds();
            return false;
        }

        if (FPlatformTime::Seconds() - Run.StageStart > 45.0)
        {
            Run.State->Fail(TEXT("The DAP client did not start capturing in time."));
            if (!Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(TEXT("The DAP capture client did not stop after the timeout."));
            }
            Run.Stage = EStage::Done;
            return true;
        }
        // Yield so the game thread can serve the source render the worker is waiting on.
        return false;
    }

    if (Run.Stage == EStage::WaitingForDone)
    {
        if (Run.State->bDone.load())
        {
            if (!Run.Worker.JoinIfFinished() && !Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(TEXT("The DAP capture client was still running when PIE teardown began."));
            }
            Run.Stage = EStage::Done;
            return true;
        }

        if (FPlatformTime::Seconds() - Run.StageStart > 45.0)
        {
            Run.State->Fail(TEXT("The DAP capture client did not finish."));
            if (!Run.Worker.StopAndJoin(10.0))
            {
                Test->AddError(TEXT("The DAP capture client did not stop after the timeout."));
            }
            Run.Stage = EStage::Done;
            return true;
        }
        return false;
    }

    return true;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapHotCaptureAssertCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapHotCaptureTest::FFixture>, Fixture);

bool FDapHotCaptureAssertCommand::Update()
{
    using namespace BlueprintDapHotCaptureTest;

    if (!Fixture->Worker.Shutdown())
    {
        Test->AddError(TEXT("The DAP capture client was still running when the test finished."));
        return true;
    }

    FState& State = *Fixture->State;
    const FString Error = State.GetError();

    Test->TestTrue(TEXT("the DAP client finished"), State.bDone.load());
    Test->TestTrue(TEXT("the DAP client authenticated"), State.bAuthenticated.load());
    Test->TestTrue(TEXT("the fixture source was rendered"), State.bRendered.load());
    Test->TestTrue(TEXT("startCapture entered capture mode"), State.bCaptureStarted.load());
    Test->TestTrue(TEXT("startCapture cleared the counters"), State.bStartCleared.load());
    Test->TestTrue(TEXT("the fixture function ran under capture"), State.bFunctionCalled.load());
    Test->TestTrue(TEXT("capture counted dispatches"), State.RawBeforeStop.load() > 0);
    Test->TestTrue(TEXT("hotLines resolved the fixture's executed lines"), State.MatchingLines.load() > 0);
    Test->TestTrue(TEXT("stopCapture kept the counters"), State.bStopKeptCounters.load());
    Test->TestTrue(TEXT("resetCapture cleared the counters"), State.bResetCleared.load());
    Test->TestTrue(TEXT("coverage listed executed statements"), State.CoverageCovered.load() > 0);
    Test->TestTrue(TEXT("coverage listed unexecuted statements"), State.CoverageUncovered.load() > 0);
    Test->TestTrue(TEXT("coverage lines carry a statement/transfer kind and a function"), State.bCoverageKinds.load());
    Test->TestTrue(TEXT("coverage totals add up to its line list"), State.bCoverageTotals.load());
    Test->TestTrue(TEXT("resetCapture left every coverage line uncovered"), State.bCoverageResetCleared.load());
    Test->TestTrue(TEXT("hotLines reported the capture's map, build, engine and start time"), State.bMetadata.load());
    if (!Error.IsEmpty())
    {
        Test->AddError(Error);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDapHotCaptureTest, "BlueprintInspector.Debugger.DapHotCapture", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDapHotCaptureTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintDapHotCaptureTest;

    TSharedPtr<BlueprintDapHotCaptureTest::FFixture> Fixture = MakeShared<BlueprintDapHotCaptureTest::FFixture>();

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(GetBlueprintTestMapPath()));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FDapHotCaptureCallCommand(this, Fixture));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FDapHotCaptureAssertCommand(this, Fixture));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
