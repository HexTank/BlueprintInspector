// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"
#include "Runtime/Launch/Resources/Version.h"
#include "UObject/NameTypes.h"

#define BI_ENGINE_AT_LEAST(Major, Minor) (ENGINE_MAJOR_VERSION > (Major) || (ENGINE_MAJOR_VERSION == (Major) && ENGINE_MINOR_VERSION >= (Minor)))
#define BI_ENGINE_AT_LEAST_5 BI_ENGINE_AT_LEAST(5, 0)
#define BI_ENGINE_AT_LEAST_5_1 BI_ENGINE_AT_LEAST(5, 1)
#define BI_ENGINE_AT_LEAST_5_2 BI_ENGINE_AT_LEAST(5, 2)
#define BI_ENGINE_AT_LEAST_5_3 BI_ENGINE_AT_LEAST(5, 3)
#define BI_ENGINE_AT_LEAST_5_4 BI_ENGINE_AT_LEAST(5, 4)
#define BI_ENGINE_AT_LEAST_5_5 BI_ENGINE_AT_LEAST(5, 5)
#define BI_ENGINE_AT_LEAST_5_6 BI_ENGINE_AT_LEAST(5, 6)
#define BI_ENGINE_AT_LEAST_5_7 BI_ENGINE_AT_LEAST(5, 7)
#define BI_ENGINE_AT_LEAST_5_8 BI_ENGINE_AT_LEAST(5, 8)

#if BI_ENGINE_AT_LEAST_5
using BI_FVector3f = FVector3f;
#else
using BI_FVector3f = FVector;
#endif

// UE 5.4 replaced TArray::Pop(bool) with EAllowShrinking; earlier minors still take the bool.
#if BI_ENGINE_AT_LEAST_5_4
#define BI_POP_NO_SHRINK EAllowShrinking::No
#else
#define BI_POP_NO_SHRINK false
#endif

// FScriptName's public API drifted across 5.x minors (FName conversion and ToString() came at different
// times). ScriptNameToName() exists in every supported engine, so both helpers go through it instead of
// version-gating individual members.
FORCEINLINE FName BI_ScriptNameToName(const FScriptName& InName)
{
    return ScriptNameToName(InName);
}

FORCEINLINE FString BI_ScriptNameToString(const FScriptName& InName)
{
    return ScriptNameToName(InName).ToString();
}
