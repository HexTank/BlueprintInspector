// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"
#include "BlueprintStructDefaults.generated.h"

class UScriptStruct;

USTRUCT()
struct FBlueprintStructDefaultsMapHolder
{
    GENERATED_BODY()

    UPROPERTY()
    TMap<FVector, int32> Entries;
};

/**
 * Shared handling for struct-literal text carried by the fixture graph archive. The archive was
 * exported by one engine, so its member names carry that engine's struct GUIDs; a transient struct has
 * its own names. Bootstrap and repair both need the same rewrite.
 */
namespace BlueprintStructDefaults
{
#if WITH_EDITOR
/**
 * Maps an archive member name ("Member_0_<32-hex-guid>") to the bare member name. This is a support
 * helper for the fixture bootstrap/repair; a name without a GUID suffix is returned unchanged.
 */
FString MemberBaseName(const FString& Name);

/**
 * Maps an archive member name to the target struct's member name. A null Struct is tolerated: the
 * stripped base name is returned unchanged. This overload does not report whether the name matched;
 * use TryTargetStructMemberName when a stale archive member must be detected.
 */
FString TargetStructMemberName(const UScriptStruct* Struct, const FString& ArchiveName);

// As TargetStructMemberName, but reports whether ArchiveName resolved to a real member of Struct.
// OutName is only the target spelling on success; on failure it holds the stripped base name.
bool TryTargetStructMemberName(const UScriptStruct* Struct, const FString& ArchiveName, FString& OutName);

/**
 * Splits a struct-literal default such as "(A=1,B=(C=2))" into its top-level members. OutMembers is
 * always reset. Returns false (leaving OutMembers empty) when Value is not wrapped in parentheses or is
 * unbalanced, e.g. empty input or a truncated literal; a true result is a complete top-level split.
 * Members are returned raw (surrounding whitespace is kept), an empty literal "()" yields one empty
 * member, and a comma inside a quoted member is not a separator.
 */
bool SplitStructMembers(const FString& Value, TArray<FString>& OutMembers);

/**
 * Rewrites the archive's member names inside a struct-literal default to the target struct's names,
 * recursing through directly nested structs and struct members held in arrays/sets/maps. A map
 * literal is rewritten per pair: the key text against the key struct and the value text against the
 * value struct, so archive names inside a struct key are rewritten too.
 *
 * A rewritten literal is re-serialized as "(" + members joined by "," + ")", so the original spacing is
 * dropped; callers that compare text see a normalized string, not their input.
 *
 * Returns Value unchanged when Struct is null, Value is not a struct literal, or the nesting exceeds the
 * rewrite depth. Depth is the current recursion depth; callers pass 0.
 */
FString RewriteStructDefault(const UScriptStruct* Struct, const FString& Value, int32 Depth = 0);
#endif // WITH_EDITOR
} // namespace BlueprintStructDefaults
