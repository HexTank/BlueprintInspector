#pragma once

#include "CoreMinimal.h"
#include "BlueprintInspectionTypes.h"
#include "UObject/WeakObjectPtr.h"

class UFunction;

struct FBreakpointRecord
{
    // The function the record was resolved against; invalid after the function is collected.
    TWeakObjectPtr<const UFunction> Function;
    FString Condition;
    FString HitCondition;
    FString LogMessage;
    // Hit conditions count every dispatch, including suppressed ones; a hot breakpoint can pass 2^31.
    int64 HitCount = 0;
};

// A setBreakpoints request that arrived while the game thread was parked. Breakpoint state is only
// mutated on the game thread, so the request is queued and applied as soon as the pause is released.
struct FBlueprintPendingBreakpoints
{
    FBlueprintSourceIdentity Identity;
    TArray<FBlueprintBreakpointRequest> Requests;
};
