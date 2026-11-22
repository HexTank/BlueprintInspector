#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && PLATFORM_WINDOWS

#include "Misc/AutomationTest.h"
#include "Tests/AutomationEditorCommon.h"

#include "Editor.h"
#include "Engine/World.h"
#include "HAL/PlatformTime.h"

#include "BlueprintInspectorDetours.h"
#include "BlueprintTestConfiguration.h"

namespace BlueprintDetourLifecycleTest
{
// The exit criterion for the detour phase: twenty install/teardown cycles while a PIE session runs.
constexpr int32 Cycles = 20;
constexpr double PieTimeoutSeconds = 60.0;

// Latent command that cycles the hooks once PIE is up. The cycles run on the game thread, where both
// entry points are allowed, and between them the VM is free to dispatch through the wrapper.
class FDetourCycleCommand : public IAutomationLatentCommand
{
public:
    explicit FDetourCycleCommand(FAutomationTestBase* InTest) : Test(InTest)
    {
    }

    bool Update() override
    {
        if (GEditor == nullptr || GEditor->PlayWorld == nullptr)
        {
            if (bCheckedStartup)
            {
                Test->AddError(FString::Printf(TEXT("PIE ended after %d of %d cycles; the detour lifecycle test could not complete."), CyclesCompleted, Cycles));
                return true;
            }
            if (PieWaitStart < 0.0)
            {
                PieWaitStart = FPlatformTime::Seconds();
            }
            if (FPlatformTime::Seconds() - PieWaitStart > PieTimeoutSeconds)
            {
                Test->AddError(FString::Printf(TEXT("PIE did not start within %.0f s; the detour lifecycle test could not run."), PieTimeoutSeconds));
                return true;
            }
            return false;
        }

        // PIE is up, so the subsystem installed the hooks when its game instance started.
        if (!bCheckedStartup)
        {
            bCheckedStartup = true;
            if (FBlueprintInspectorDetours::TestCountWrappedEntries() <= 0)
            {
                Test->AddError(TEXT("The dispatch table was not wrapped when PIE started."));
                return true;
            }
        }

        const int32 Cycle = CyclesCompleted;

        FBlueprintInspectorDetours::Deinitialize();
        if (FBlueprintInspectorDetours::IsInitialized())
        {
            return FailWithHooksRestored(FString::Printf(TEXT("Cycle %d: teardown still reports initialized."), Cycle));
        }
        if (const int32 Wrapped = FBlueprintInspectorDetours::TestCountWrappedEntries(); Wrapped != 0)
        {
            return FailWithHooksRestored(FString::Printf(TEXT("Cycle %d: teardown left %d dispatch entries pointing into the module."), Cycle, Wrapped));
        }

        // Arming while torn down must not publish an enabled state: nothing would observe it.
        FBlueprintInspectorDetours::SetEnabled(true);
        if (FBlueprintInspectorDetours::IsEnabled())
        {
            return FailWithHooksRestored(FString::Printf(TEXT("Cycle %d: debugging was armed with no hooks installed."), Cycle));
        }

        FBlueprintInspectorDetours::Initialize();
        FBlueprintInspectorDetours::Activate();
        if (!FBlueprintInspectorDetours::IsInitialized())
        {
            return FailWithHooksRestored(FString::Printf(TEXT("Cycle %d: re-initialize failed."), Cycle));
        }
        if (FBlueprintInspectorDetours::TestCountWrappedEntries() <= 0)
        {
            return FailWithHooksRestored(FString::Printf(TEXT("Cycle %d: re-initialize left the dispatch table unwrapped."), Cycle));
        }

        ++CyclesCompleted;
        return CyclesCompleted >= Cycles;
    }

private:
    bool FailWithHooksRestored(const FString& Message)
    {
        Test->AddError(Message);
        if (!FBlueprintInspectorDetours::IsInitialized())
        {
            FBlueprintInspectorDetours::Initialize();
        }
        FBlueprintInspectorDetours::Activate();
        return true;
    }

    FAutomationTestBase* Test = nullptr;
    double PieWaitStart = -1.0;
    int32 CyclesCompleted = 0;
    bool bCheckedStartup = false;
};
} // namespace BlueprintDetourLifecycleTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDetourLifecycleTest, "BlueprintInspector.Debugger.DetourLifecycle", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDetourLifecycleTest::RunTest(const FString& Parameters)
{
    ADD_LATENT_AUTOMATION_COMMAND(FEditorLoadMap(GetBlueprintTestMapPath()));
    ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
    ADD_LATENT_AUTOMATION_COMMAND(BlueprintDetourLifecycleTest::FDetourCycleCommand(this));
    ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
    return true;
}

namespace BlueprintDetourConsumerTest
{
struct FDummyHandler : public UObject
{
    void Step(const UObject*, const FFrame&) {}
    void Jump(const UObject*, const FFrame&, int64) {}
};
} // namespace BlueprintDetourConsumerTest

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintDetourConsumerTest, "BlueprintInspector.Debugger.DetourConsumerGeneration", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintDetourConsumerTest::RunTest(const FString& Parameters)
{
    using FDummyHandler = BlueprintDetourConsumerTest::FDummyHandler;

    FBlueprintInspectorDetours::UnbindScriptStep();
    FBlueprintInspectorDetours::UnbindScriptJump();

    const uint32 Before = FBlueprintInspectorDetours::TestConsumerGeneration();

    TestFalse(TEXT("a null step owner is rejected"), FBlueprintInspectorDetours::BindScriptStep<FDummyHandler>(nullptr, &FDummyHandler::Step));
    TestFalse(TEXT("a null jump owner is rejected"), FBlueprintInspectorDetours::BindScriptJump<FDummyHandler>(nullptr, &FDummyHandler::Jump));
    TestEqual(TEXT("a null owner is not bound"), FBlueprintInspectorDetours::TestConsumerGeneration(), Before);

    FBlueprintInspectorDetours::UnbindScriptStep();
    FBlueprintInspectorDetours::UnbindScriptJump();
    TestEqual(TEXT("an unbind with nothing bound leaves the generation alone"), FBlueprintInspectorDetours::TestConsumerGeneration(), Before);

    FBlueprintInspectorDetours::RemoveScriptStepObserver(FDelegateHandle());
    FBlueprintInspectorDetours::RemoveScriptJumpObserver(FDelegateHandle());
    TestEqual(TEXT("removing an unknown observer leaves the generation alone"), FBlueprintInspectorDetours::TestConsumerGeneration(), Before);

    const FDelegateHandle Handle = FBlueprintInspectorDetours::AddScriptStepObserver(FBlueprintInspectorDetours::FScriptStepObserved::FDelegate::CreateLambda([](const UFunction*, int64) {}));
    const uint32 AfterAdd = FBlueprintInspectorDetours::TestConsumerGeneration();
    TestTrue(TEXT("adding an observer bumps the generation"), AfterAdd != Before);

    FBlueprintInspectorDetours::RemoveScriptStepObserver(Handle);
    const uint32 AfterRemove = FBlueprintInspectorDetours::TestConsumerGeneration();
    TestTrue(TEXT("removing the observer bumps the generation"), AfterRemove != AfterAdd);

    FBlueprintInspectorDetours::RemoveScriptStepObserver(Handle);
    TestEqual(TEXT("removing the same observer twice bumps once"), FBlueprintInspectorDetours::TestConsumerGeneration(), AfterRemove);
    return true;
}

#endif
