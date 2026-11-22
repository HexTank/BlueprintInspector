// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
namespace BlueprintInspectorTests
{
/**
 * Drops the hand-built UFunction fixtures the automation tests keep alive at file scope. The plugin
 * module calls this on shutdown so they are destroyed while the UObject system is alive, instead of
 * being left to a static destructor at process exit.
 */
void ReleaseScriptFunctions();
} // namespace BlueprintInspectorTests
#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
