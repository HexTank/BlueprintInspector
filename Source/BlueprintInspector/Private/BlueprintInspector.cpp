// Copyright (c) 2024-2026 Paul Tankard

#include "BlueprintInspector.h"

#if PLATFORM_WINDOWS
#include "BlueprintInspectorDetours.h"
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "Misc/CoreDelegates.h"
#include "Tests/BlueprintFixtureSource.h"
#include "Tests/BlueprintTestKeepAlives.h"
#endif

DEFINE_LOG_CATEGORY(LogBlueprintInspector);

void FBlueprintInspectorModule::StartupModule()
{
    UE_LOG(LogBlueprintInspector, Log, TEXT("BlueprintInspector::StartupModule"));

#if PLATFORM_WINDOWS
    // Detours needs a trampoline within 2 GB of UWorld::Tick. Attaching here, before the editor loads
    // its full module set, gives that allocation the best chance; the subsystem still retries when this
    // attempt loses the space to a module loaded in between.
    if (IsInGameThread())
    {
        FBlueprintInspectorDetours::Initialize();
    }
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
    TestFixtureReleaseHandle = FCoreDelegates::OnPreExit.AddLambda([] {
        BlueprintInspectorTests::ReleaseScriptFunctions();
        BlueprintFixtureSource::Release();
    });
#endif
}

void FBlueprintInspectorModule::ShutdownModule()
{
#if PLATFORM_WINDOWS
    // A module unload with the hooks still installed would leave jumps aimed at freed code. The
    // subsystem also deinitializes; this call is idempotent.
    if (IsInGameThread())
    {
        FBlueprintInspectorDetours::Deinitialize();
    }
    else
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("BlueprintInspector::ShutdownModule ran off the game thread; the UWorld hooks were not detached."));
    }
#endif

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
    if (TestFixtureReleaseHandle.IsValid())
    {
        FCoreDelegates::OnPreExit.Remove(TestFixtureReleaseHandle);
        TestFixtureReleaseHandle.Reset();
    }

    // The pre-exit hook releases on the game thread. This fallback covers a module unload that skips
    // the hook, and it stays on the game thread to match the setup that created the fixtures.
    if (IsEngineExitRequested() && IsInGameThread())
    {
        BlueprintInspectorTests::ReleaseScriptFunctions();
        BlueprintFixtureSource::Release();
    }
#endif

    UE_LOG(LogBlueprintInspector, Log, TEXT("BlueprintInspector::ShutdownModule"));
}

IMPLEMENT_MODULE(FBlueprintInspectorModule, BlueprintInspector)
