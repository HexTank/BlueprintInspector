// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && PLATFORM_WINDOWS

#include "Misc/AutomationTest.h"

#include "BlueprintInspectorDetours.h"
#include "BlueprintInspectorPort.h"
#include "BlueprintInspectorSubsystem.h"

#include "Engine/GameInstance.h"
#include "Subsystems/SubsystemCollection.h"
#include "UObject/StrongObjectPtr.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintSubsystemOwnershipTest, "BlueprintInspector.Debugger.SubsystemOwnership", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintSubsystemOwnershipTest::RunTest(const FString& Parameters)
{
    if (UBlueprintInspectorSubsystem::HasReferenceOwnerForTest())
    {
        AddError(TEXT("another subsystem already owns the reference; this test needs a free owner"));
        return false;
    }

    UGameInstance* const GameInstance = NewObject<UGameInstance>();
    TStrongObjectPtr<UGameInstance> GameInstanceGuard(GameInstance);
    UBlueprintInspectorSubsystem* const Dummy = NewObject<UBlueprintInspectorSubsystem>(GameInstance);
    TStrongObjectPtr<UBlueprintInspectorSubsystem> DummyGuard(Dummy);
    UBlueprintInspectorSubsystem* const Subsystem = NewObject<UBlueprintInspectorSubsystem>(GameInstance);
    TStrongObjectPtr<UBlueprintInspectorSubsystem> SubsystemGuard(Subsystem);
    if (!TestNotNull(TEXT("game instance"), GameInstance) || !TestNotNull(TEXT("dummy owner"), Dummy) || !TestNotNull(TEXT("subsystem"), Subsystem))
    {
        return false;
    }

    TestTrue(TEXT("the dummy owns the script streams"), UBlueprintInspectorSubsystem::BindScriptHandlersForTest(Dummy));

#if BI_ENGINE_AT_LEAST_5_4
    AddExpectedErrorPlain(TEXT("BlueprintInspector could not bind the script handlers"), EAutomationExpectedErrorFlags::Contains, 1);
#else
    AddExpectedError(TEXT("BlueprintInspector could not bind the script handlers"), EAutomationExpectedErrorFlags::Contains, 1);
#endif

    FSubsystemCollection<UGameInstanceSubsystem> Collection;
    Subsystem->Initialize(Collection);

    TestFalse(TEXT("a failed initialization releases the reference owner"), Subsystem->IsReferenceOwnerForTest());

    UBlueprintInspectorSubsystem::ClearReferenceOwnerForTest(Subsystem);
    FBlueprintInspectorDetours::UnbindScriptStep();
    FBlueprintInspectorDetours::UnbindScriptJump();
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR && PLATFORM_WINDOWS
