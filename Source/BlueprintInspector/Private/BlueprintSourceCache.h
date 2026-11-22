#pragma once

#include "CoreMinimal.h"
#include "BlueprintSourceMap.h"
#include "HAL/CriticalSection.h"

// Rendered sources keyed by the server-issued class handle. Every read copies under the lock and
// never renders, lists classes, or touches engine state, so a request may call them while the game
// thread is parked; a returned copy cannot be invalidated by a later store or invalidate.
class FBlueprintSourceCache
{
public:
    bool TryCopy(int32 Handle, FString& OutText) const;
    bool TryCopyMap(int32 Handle, FBlueprintSourceMap& OutMap) const;
    void Store(int32 Handle, FBlueprintRenderedSource&& Rendered);
    void Invalidate(int32 Handle);
    void Clear();
    TOptional<FBlueprintRenderedSource> Find(int32 Handle) const;

private:
    // Must be called while holding SourcesLock.
    const FBlueprintRenderedSource* FindLocked(int32 Handle) const;

    // Readers copy under the read lock, so concurrent requests do not serialize on each other; a store
    // or invalidate still takes the write lock for the whole replacement.
    mutable FRWLock SourcesLock;
    TMap<int32, TUniquePtr<FBlueprintRenderedSource>> Sources;
};
