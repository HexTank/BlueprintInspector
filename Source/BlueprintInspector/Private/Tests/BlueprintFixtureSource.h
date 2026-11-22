// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"

class UBlueprint;
class UClass;
class UFunction;

/**
 * The suite's fixture functions come from two transient blueprints:
 *
 * - the main transient blueprint (every archive graph except the three struct rows);
 * - the struct transient blueprint (const_struct, structconst_containers, let_struct).
 *
 * The transient set is built once per process. Callers use GetAllFunctions/FindFunction instead of
 * walking one generated class, so a row can move between the sources without a test edit.
 */
namespace BlueprintFixtureSource
{
#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
/** The package root the transient fixtures are built under. */
const TCHAR* GetTransientRoot();

/**
 * The main transient blueprint, or null if it could not be built. The pointer is the single process-wide
 * fixture instance; callers must treat it as read-only so later tests see the same asset.
 *
 * All accessors in this namespace must be called from the game thread: building the fixtures creates
 * UObjects. The implementation states that affinity with a check, so it is enforced only in builds that
 * compile checks in (DO_CHECK != 0) and is not verified at all in a Test/Shipping-style configuration.
 */
UBlueprint* GetMain();

/**
 * The struct-literal transient blueprint, or null if it could not be built. Same shared-instance rule as
 * GetMain().
 */
UBlueprint* GetStructs();

/**
 * Drops both transient blueprints and disables the fixtures permanently: after a release on the game
 * thread GetMain(), GetStructs() and GetAllFunctions() return null/empty instead of rebuilding, so no
 * UObjects are created during teardown. The plugin module calls this on shutdown; it is not intended to
 * be undone.
 *
 * Called from another thread it only disables rebuilding: the release is logged and the blueprints are
 * left in place, so the accessors still return them.
 */
void Release();

/** Generated class of the main transient fixture. */
UClass* GetMainClass();

/**
 * A prerequisite class from the transient root, e.g. pass BP_ProbeCastTarget (the _C suffix is added
 * here). Returns null when Name is null/empty or no matching class was built.
 */
UClass* GetPrerequisiteClass(const TCHAR* Name);

/**
 * Every fixture function across the two transient sources, deduplicated per class (the struct fixture
 * derives from the main one, so a same-named function there is an override and is kept).
 *
 * The elements are non-owning pointers into the shared fixtures and are only valid until Release();
 * callers must not mutate the functions or keep the array across a release.
 */
TArray<UFunction*> GetAllFunctions();

/**
 * Resolves one function by name across the two transient sources. The struct fixture's own function wins
 * over a same-named function in the main fixture (it overrides it); otherwise the main fixture's is used.
 */
UFunction* FindFunction(FName Name);
#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
} // namespace BlueprintFixtureSource
