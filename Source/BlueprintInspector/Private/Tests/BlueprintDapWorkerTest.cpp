// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "BlueprintDapTestClient.h"
#include "HAL/PlatformProcess.h"

#include <atomic>

namespace BlueprintDapWorkerTest
{
using namespace BlueprintDapTest;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FDapWorkerRestartTest, "BlueprintInspector.Debugger.DapWorkerRestart", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FDapWorkerRestartTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintDapWorkerTest;

    TSharedPtr<FClientState> State = MakeShared<FClientState>();
    FWorker Worker;
    const std::atomic<bool>& Stop = Worker.Stop();

    std::atomic<int32> Runs{0};

    TestTrue(TEXT("the first client starts"), Worker.Start(State, TSharedPtr<const void>(), [&Runs]() { ++Runs; }));
    TestTrue(TEXT("the first client body returns"), Worker.WaitForExit(5.0));
    TestTrue(TEXT("the first client thread is joined"), Worker.JoinIfFinished(5.0));

    TestTrue(TEXT("a restart starts a second client"), Worker.Start(State, TSharedPtr<const void>(), [&Runs]() { ++Runs; }));
    TestTrue(TEXT("the second client body returns"), Worker.WaitForExit(5.0));
    TestEqual(TEXT("both bodies ran"), Runs.load(), 2);
    TestTrue(TEXT("the second client thread is joined"), Worker.JoinIfFinished(5.0));

    TestTrue(TEXT("the third client starts"), Worker.Start(State, TSharedPtr<const void>(), [&Stop]() {
        while (!Stop.load())
        {
            FPlatformProcess::Sleep(0.001f);
        }
    }));
    TestEqual(TEXT("no client failure was recorded before the deliberate double start"), State->GetError(), FString());
    TestFalse(TEXT("a start while the client is running is refused"), Worker.Start(State, TSharedPtr<const void>(), []() {}));
    TestTrue(TEXT("the refusal names the cause"), State->GetError().Contains(TEXT("started twice")));

    TestTrue(TEXT("the client shuts down"), Worker.Shutdown());
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
