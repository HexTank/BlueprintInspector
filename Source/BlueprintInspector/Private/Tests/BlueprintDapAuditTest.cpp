// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

#include "Editor.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include "BlueprintDapTestClient.h"
#include "BlueprintInspectorDAP.h"
#include "BlueprintTestConfiguration.h"

#include "dap/network.h"
#include "dap/session.h"

#include <atomic>
#include <memory>

namespace BlueprintDapAuditTest
{
using namespace BlueprintDapTest;

struct FState : FClientState
{
    std::atomic<bool> bSuccess{false};
    std::atomic<int32> Classes{0};
    std::atomic<int32> Functions{0};
    std::atomic<int32> Findings{0};
    std::atomic<bool> bHasProjectFinding{false};
};

// One client per test run.
struct FFixture
{
    TSharedPtr<FState> State = MakeShared<FState>();
    FWorker Worker;

    bool bStarted = false;
    bool bStopAttempted = false;
    double PieWaitStart = -1.0;
    double WorkerStart = -1.0;
};

void RunClient(const FString& TokenPath, FFixture& Fixture)
{
    FState& State = *Fixture.State;
    const std::atomic<bool>& Stop = Fixture.Worker.Stop();

    std::shared_ptr<dap::ReaderWriter> ReaderWriter = Connect(Stop, State);
    if (!ReaderWriter)
    {
        // The done guard is declared after the session so the flag rises before the socket closes; this
        // early exit precedes it, so raise the flag here or the assert command waits on a dead client.
        State.bDone.store(true);
        return;
    }

    std::unique_ptr<dap::Session> Session = dap::Session::create();
    Session->bind(ReaderWriter, []() {});

    // Declared after the session so the completion flag is raised before the socket closes.
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

    dap::BlueprintAuditRequest AuditRequest;
    AuditRequest.maxFindings = 5000;
    dap::BlueprintAuditResponse Response;
    if (!Send(*Session, AuditRequest, Response, State, Stop, TEXT("audit"), 120.0))
    {
        return;
    }

    if (Response.truncated)
    {
        State.Fail(FString::Printf(TEXT("The audit stopped at its finding budget after %d finding(s), so the coverage assertions below would pass on a partial scan."), static_cast<int32>(Response.findings.size())));
    }

    State.Classes.store(static_cast<int32>(Response.classes));
    State.Functions.store(static_cast<int32>(Response.functions));
    State.Findings.store(static_cast<int32>(Response.findings.size()));
    for (const dap::BlueprintAuditFinding& Finding : Response.findings)
    {
        if (Finding.rule == "BPI001")
        {
            State.bHasProjectFinding.store(true);
        }
        if (Finding.severity != "error" && Finding.severity != "warning" && Finding.severity != "info")
        {
            State.Fail(FString::Printf(TEXT("unexpected severity '%s'"), UTF8_TO_TCHAR(Finding.severity.c_str())));
            return;
        }
    }

    State.bSuccess.store(true);
}
} // namespace BlueprintDapAuditTest

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapAuditCallCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapAuditTest::FFixture>, Fixture);

bool FDapAuditCallCommand::Update()
{
    using namespace BlueprintDapAuditTest;

    if (!Fixture->bStarted)
    {
        if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
        {
            if (Fixture->PieWaitStart < 0.0)
            {
                Fixture->PieWaitStart = FPlatformTime::Seconds();
            }
            if (FPlatformTime::Seconds() - Fixture->PieWaitStart > 60.0)
            {
                Test->AddError(TEXT("PIE did not start; the DAP audit test could not run."));
                return true;
            }
            return false;
        }

        const FString TokenPath = FUBlueprintInspectorDap::GetTokenFilePath();
        // A token left behind by an interrupted run would otherwise be read instead of this run's.
        if (IFileManager::Get().FileExists(*TokenPath) && !IFileManager::Get().Delete(*TokenPath, false, true, true))
        {
            Test->AddError(TEXT("The stale DAP token file could not be deleted."));
            return true;
        }

        FFixture* const FixturePtr = Fixture.Get();
        if (!Fixture->Worker.Start(Fixture->State, Fixture, [FixturePtr, TokenPath]() { RunClient(TokenPath, *FixturePtr); }))
        {
            Test->AddError(TEXT("The DAP audit client thread could not start."));
            return true;
        }

        Fixture->bStarted = true;
        Fixture->WorkerStart = FPlatformTime::Seconds();
        return false;
    }

    if (Fixture->State->bDone.load() || Fixture->Worker.WaitForExit(0.0))
    {
        if (!Fixture->Worker.JoinIfFinished())
        {
            Test->AddError(TEXT("The DAP audit client did not exit after its body returned."));
        }
        return true;
    }

    if (FPlatformTime::Seconds() - Fixture->WorkerStart > 240.0)
    {
        Fixture->State->Fail(TEXT("The DAP audit client did not finish."));

        if (!Fixture->bStopAttempted)
        {
            Fixture->bStopAttempted = true;
            if (Fixture->Worker.StopAndJoin(10.0))
            {
                return true;
            }
            Fixture->Worker.RequestStop();
            return false;
        }

        const bool bExited = Fixture->State->bDone.load() && Fixture->Worker.JoinIfFinished(1.0);
        if (!bExited)
        {
            Test->AddError(TEXT("The DAP audit client was still running when the test timed out."));
            if (!Fixture->Worker.StopAndJoin(5.0))
            {
                Test->AddError(TEXT("The DAP audit client could not be stopped before PIE teardown."));
            }
        }
        return true;
    }

    // Yield so the game thread can serve RunOnGameThread for the audit.
    return false;
}

DEFINE_LATENT_AUTOMATION_COMMAND_TWO_PARAMETER(FDapAuditAssertCommand, FAutomationTestBase*, Test, TSharedPtr<BlueprintDapAuditTest::FFixture>, Fixture);

bool FDapAuditAssertCommand::Update()
{
    using namespace BlueprintDapAuditTest;

    // A run that never started already reported its cause; assert nothing on top of it.
    if (!Fixture->bStarted)
    {
        return true;
    }

    FState& State = *Fixture->State;

    // The call command joins on every path it owns; this covers the client still exiting when it did.
    if (!Fixture->Worker.Shutdown())
    {
        Test->AddError(TEXT("The DAP audit client was still running when the test finished."));
    }

    // Read after the shutdown so a failure raised while the worker unwinds is reported.
    const FString Error = State.GetError();

    Test->TestTrue(TEXT("the DAP client finished"), State.bDone.load());
    Test->TestTrue(TEXT("the audit request succeeded"), State.bSuccess.load());
    Test->TestTrue(TEXT("classes were scanned"), State.Classes.load() > 0);
    Test->TestTrue(TEXT("functions were scanned"), State.Functions.load() > 0);
    Test->TestTrue(TEXT("the audit returned findings"), State.Findings.load() > 0);
    Test->TestTrue(TEXT("the shipping finding is present"), State.bHasProjectFinding.load());
    if (!Error.IsEmpty())
    {
        Test->AddError(Error);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDapAuditTest, "BlueprintInspector.Debugger.DapAudit", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDapAuditTest::RunTest(const FString& Parameters)
{
    TSharedPtr<BlueprintDapAuditTest::FFixture> Fixture = MakeShared<BlueprintDapAuditTest::FFixture>();

    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(GetBlueprintTestMapPath()));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(FDapAuditCallCommand(this, Fixture));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    ADD_LATENT_AUTOMATION_COMMAND(FDapAuditAssertCommand(this, Fixture));
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
