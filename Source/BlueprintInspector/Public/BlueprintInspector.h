// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"
#include "Delegates/Delegate.h"
#include "Logging/LogMacros.h"
#include "Modules/ModuleManager.h"

#if UE_BUILD_SHIPPING
// Shipping keeps Log and above; every Verbose call site compiles out.
BLUEPRINTINSPECTOR_API DECLARE_LOG_CATEGORY_EXTERN(LogBlueprintInspector, Log, Log);
#else
BLUEPRINTINSPECTOR_API DECLARE_LOG_CATEGORY_EXTERN(LogBlueprintInspector, Log, All);
#endif

class BLUEPRINTINSPECTOR_API FBlueprintInspectorModule final : public IModuleInterface
{
public:
    void StartupModule() override;
    void ShutdownModule() override;

private:
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
    FDelegateHandle TestFixtureReleaseHandle;
#endif
};
