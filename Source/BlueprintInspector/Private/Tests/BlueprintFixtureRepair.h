// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"

class UBlueprint;

/**
 * Repairs a saved fixture blueprint after the editor has loaded it. The automation suite now builds
 * its fixtures in memory (BlueprintFixtureSource); this is the graph tools' load/repair path for the
 * saved per-engine asset.
 *
 * On 5.5-5.7 the editor recompiles a freshly saved fixture on load and resets its call-pin defaults to
 * the function's defaults (zero/None) before it is inspected. The graph archive is the source of
 * truth, so the tools re-apply those defaults and compile once, rather than depending on engine load
 * behavior. On engines that keep the defaults (5.8) the repair detects the correct value and does
 * nothing.
 */
namespace BlueprintFixtureRepair
{
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
/**
 * Loads BP_OpcodeDemo_All and repairs it if the engine reset its pin defaults. Returns nullptr when the
 * asset cannot be loaded. A non-null result is not a guarantee that the repair succeeded: the asset may
 * still be in BS_Error or keep reset defaults (the failure is logged), so callers should check
 * Result->Status and the pins before trusting its content.
 */
UBlueprint* LoadRepairedFixture();

/**
 * Applies the archive's pin defaults to a loaded fixture, recompiles it and verifies the defaults against
 * the archive. Game thread only. No-op when Fixture is null or already has the archive's defaults; the
 * package is never saved. Any repair attempt marks the package dirty (structural modify + compile), even
 * if the archive defaults still cannot be restored.
 *
 * Returns true only when the fixture verifies correct afterwards: it is true for the no-op path, and false
 * when the source is null, the archive cannot be read, the blueprint fails to compile, or the defaults
 * are still reset. The failure is logged.
 */
bool RepairLoadedFixture(UBlueprint* Fixture);
#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
} // namespace BlueprintFixtureRepair
