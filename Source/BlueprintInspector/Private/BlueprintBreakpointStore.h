#pragma once

#include "CoreMinimal.h"
#include "BlueprintBreakpointTypes.h"
#include "BlueprintClassRegistry.h"
#include "BlueprintInspectionTypes.h"
#include "BlueprintKeys.h"
#include "BlueprintSourceMap.h"

class UClass;
class UFunction;
class UPackage;

/** The result of one breakpoint toggle: rejected before it touched the store, or the new armed state. */
enum class EBreakpointToggle : uint8
{
    Rejected,
    Cleared,
    Set
};

// The armed breakpoints: one record per (class, function, address), the break/continue dispatch pairs,
// and an address index for the dispatch hot path.
//
// Game-thread only, like the VM that reads it. RebuildIndex replaces the address lists, so a reader must
// not hold a pointer into them across a rebuild.
//
// The setBreakpoints requests that arrive while the game thread is parked are NOT here. That queue is
// pause-protocol state and stays with the pause handshake, because "queue it" and "resume" have to be one
// transition under one lock; splitting them would let a request queue for a pause that already ended.
class FBlueprintBreakpointStore
{
public:
    explicit FBlueprintBreakpointStore(FBlueprintClassRegistry& InRegistry);

    int32 NumRecords() const
    {
        check(IsInGameThread());
        return Records.Num();
    }
    // Fills OutKey with the record's key so a caller can log the class/function/address it matched.
    // The returned pointer is invalidated by any mutation (Toggle, Apply, RebindClass, Clear); re-find
    // the record after mutating instead of holding it across one.
    FBreakpointRecord* FindRecord(const UFunction* Function, int64 Address, FBreakpointKey& OutKey);

    bool IsSet(const UFunction* Function, int64 Address) const;
    // Game thread: arms or clears one address and rebuilds the address index before returning.
    // A rejected address (collected or native function, address outside the script) leaves the store
    // unchanged and never reads as a clear.
    EBreakpointToggle Toggle(const TWeakObjectPtr<const UFunction>& Function, int64 Address);

    // The armed break/continue pairs live outside the address index, keyed by the dispatch they wait for.
    int32 NumTransfers() const { return Transfers.Num(); }
    bool ContainsTransfer(const UFunction* Function, int64 JumpAddress, int64 Landing) const;

    // Game thread: a retargeted class re-points this class's records at its current functions, dropping
    // records that no longer address a byte of the new script.
    void RebindClass(const UClass* Class);

    // Game thread: resolve a setBreakpoints request against the class's rendered lines and replace this
    // source's whole set, options included. The result is index-aligned with Requests (one entry per
    // request, never filtered), because the per-request options are written back by index.
    TArray<FBlueprintBreakpointResolution> Apply(const FBlueprintSourceIdentity& Identity, const TArray<FBlueprintBreakpointRequest>& Requests, const UPackage* PIEPackage);

    // While the game thread is parked: cached lines only, never a render. The caller queues the same
    // request for the resume, which applies it against the full line set.
    TArray<FBlueprintBreakpointResolution> ResolveCached(const FBlueprintSourceIdentity& Identity, const TArray<FBlueprintBreakpointRequest>& Requests) const;

    // Teardown: nothing may stay armed once the detours are gone.
    void Clear();

private:
    void RebuildIndex();
    bool IsArmed(const FBreakpointKey& Key) const;
    // As Toggle, without the index rebuild; Apply uses it per line and rebuilds once at the end.
    EBreakpointToggle ToggleNoIndex(const TWeakObjectPtr<const UFunction>& Function, int64 Address);

    // Line resolution and cached source maps go through the registry; the store owns no render.
    FBlueprintClassRegistry& Registry;

    TMap<FBreakpointKey, FBreakpointRecord> Records;

    // A transfer line owns no entry in Records, so the two never collide.
    TSet<FBreakpointTransferKey> Transfers;

    // Rebuilt whenever Records changes: the addresses armed per function, for the dispatch hot path.
    TMap<TWeakObjectPtr<const UFunction>, TArray<int64>> Addresses;
};
