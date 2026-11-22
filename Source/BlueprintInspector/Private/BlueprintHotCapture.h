#pragma once

#include "CoreMinimal.h"
#include "BlueprintClassRegistry.h"
#include "BlueprintInspectionTypes.h"
#include "BlueprintKeys.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"

class UFunction;

// The hot-path capture: per-address and per-transfer counters, a generation that invalidates a stale
// snapshot, and the metadata that labels a run.
//
// Threading: the flag and the generation are atomic, so a DAP thread reads them while the VM counts. The
// counters live under HotLock: the count path holds it for one increment, and a snapshot copies under it
// and resolves outside it, so a counting VM never waits on the source cache. Start/Reset/Clear bump the
// generation under the same lock, so a report cannot pair a new generation with the previous counters.
// HotLock is taken before MetadataLock, never after it.
class FBlueprintHotCapture
{
public:
    explicit FBlueprintHotCapture(FBlueprintClassRegistry& InRegistry);

    // Any thread: the three fields are read individually, so a concurrent Start/Reset can be observed
    // half-applied; take HotLock-based reports when the fields must agree.
    FBlueprintHotCaptureStatus Query() const;
    // The metadata for the generation current when the call starts, if it has been filled. Takes
    // MetadataLock internally; a Start/Reset that lands during the call is not reflected in the result.
    bool TryCopyMetadata(FBlueprintCaptureMetadata& OutMetadata) const;
    // Game thread: fill and cache the metadata for the current generation. Returns a default-constructed
    // struct when a Start/Reset bumps the generation during the build; a caller must treat a blank Map or
    // Build as no label yet rather than label a snapshot with it.
    FBlueprintCaptureMetadata AcquireMetadata();

#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> TestDuringMetadataBuild;
#endif

    FBlueprintHotCaptureStatus Start();
    // Disables under HotLock, so a count already inside the critical section finishes before this
    // returns and a report taken afterwards cannot observe one in flight.
    FBlueprintHotCaptureStatus Stop();
    FBlueprintHotCaptureStatus Reset();
    // Teardown: the detours are going away, so nothing may stay counted or enabled.
    void Clear();

    // Any thread the Blueprint VM may execute on (game thread or a worker thread): the hot path.
    // No-ops unless a client started a capture. A null Function is dropped rather than dereferenced.
    void CountStep(const UFunction* Function, int64 Address);
    void CountTransfer(const UFunction* Function, int64 JumpAddress, int64 Landing);

    // Any thread: resolve the counters against the cached renders. Only classes with a cached render can
    // be resolved; the rest are reported as neither covered nor uncovered. MaxLines is a hard cap on the
    // returned array (clamped to 1..MaxHotLinesResponse); bTruncated says whether the cap cut the report.
    static constexpr int32 MaxHotLinesResponse = 20000;

    FBlueprintHotLinesResult AcquireHotLines(int32 MaxLines) const;
    FBlueprintCoverageResult AcquireCoverage(int32 MaxLines) const;

private:
    void BumpGeneration();

    // The renders and class handles the reports resolve against. The capture owns no source itself.
    FBlueprintClassRegistry& Registry;

    mutable FCriticalSection HotLock;
    TMap<FBlueprintHotHitKey, int64> Hits;
    TMap<FBlueprintHotTransferKey, int64> TransferHits;
    TAtomic<bool> bEnabled{false};
    TAtomic<uint32> Generation{0};
    TAtomic<int64> TotalHits{0};

    // Keyed by generation: a new start or reset invalidates the cache, so a later capture never reports
    // the map the earlier one ran in.
    mutable FCriticalSection MetadataLock;
    FBlueprintCaptureMetadata MetadataCache;
    uint32 MetadataGeneration = 0;
    FString GenerationStartedAt;
    // A fresh capture has Generation 0 and no metadata; without this flag TryCopyMetadata would match
    // the initial generation and hand out the default-constructed cache.
    bool bMetadataValid = false;
};
