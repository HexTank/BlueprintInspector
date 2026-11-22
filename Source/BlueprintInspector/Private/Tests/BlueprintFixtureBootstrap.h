// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "HAL/Platform.h"

class UBlueprint;

#if WITH_EDITOR
namespace BlueprintFixtureBootstrap
{
/**
 * Creates or refreshes the opcode fixture assets for the running engine at
 * /BlueprintInspector/Tests/OpcodeShowcase. The asset shapes come from
 * Data/BlueprintLayoutGuide.md and the function bodies come from the shared graph archive.
 *
 * It also creates /Game/NewMap when the host project has no map: the suite's level-script and runtime
 * tests load it, and the level script is authored with a BeginPlay body that calls
 * GameplayStatics::FinishSpawningActor. The map is generated from code, not tracked: a level asset is
 * engine-version fragile and the plugin should not ship host content. A marker next to the map records
 * the authoring version and engine, so a generated map is refreshed after a shape or engine change;
 * a host map without the marker is left alone. The level-script test accepts any non-empty BeginPlay
 * body, so a host map only needs a level script with a statement. An explicit
 * -BlueprintInspectorTestMap= leaves the map alone.
 *
 * Invoke from the editor command line:
 *   -ExecCmds="BlueprintInspector.BootstrapFixture"
 *
 * The console command defers Run() to the next core tick and then exits the process itself, with a
 * non-zero status when Run() failed, so no Quit command may be chained after it.
 *
 * Returns true when the fixture asset was compiled without errors and saved. A failed archive graph import
 * aborts Run()/BuildTransientFixtures() (the operation returns false); only the graphs in
 * GetBlueprintGraphsTheBootstrapSkips() are skipped and merely logged. A missing test map is not fatal
 * here. Run() deletes every existing Content/Tests/OpcodeShowcase asset before authoring and switches
 * the editor level through NewBlankMap; anything else already written to disk is left in place.
 *
 * Game thread only: both functions create and unload packages and mutate the file-scope fixture state,
 * and neither asserts thread affinity, so the caller has to guarantee it. A call while another one is
 * in progress is rejected (returns false) instead of running concurrently. BuildTransientFixtures()
 * temporarily repoints FixtureRoot at the transient root and disables saving for its duration, and
 * restores both on every exit path.
 */
bool Run();

/**
 * Builds the hybrid fixture set under /Temp/BlueprintInspectorFixture without saving.
 *
 * Both outputs are nulled at entry. OutMain carries every archive graph except the struct rows, and
 * OutStructs carries the three struct-literal rows.
 *
 * Returns false when the prerequisites cannot be built, and also when the main fixture's variables or
 * dispatch delegate cannot be ensured; the main fixture is not returned in that case.
 * Returns true when the main fixture was built even if the struct fixture could not be created: OutMain
 * is valid and OutStructs stays null, so the struct rows are absent rather than the whole build failed.
 * A successful build of the main fixture may also have skipped or failed individual archive graphs;
 * that is only logged.
 *
 * The blueprints live in transient `/Temp` packages and are not rooted. A caller must use them within
 * the immediate scope (or root them) and must not hold the pointers across a GC.
 */
bool BuildTransientFixtures(UBlueprint*& OutMain, UBlueprint*& OutStructs);
} // namespace BlueprintFixtureBootstrap
#endif
