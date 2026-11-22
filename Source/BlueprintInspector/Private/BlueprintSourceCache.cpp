#include "BlueprintSourceCache.h"

#include "Misc/ScopeRWLock.h"

const FBlueprintRenderedSource* FBlueprintSourceCache::FindLocked(const int32 Handle) const
{
    const TUniquePtr<FBlueprintRenderedSource>* const Existing = Sources.Find(Handle);
    return Existing != nullptr && Existing->IsValid() ? Existing->Get() : nullptr;
}

bool FBlueprintSourceCache::TryCopy(const int32 Handle, FString& OutText) const
{
    FRWScopeLock Lock(SourcesLock, SLT_ReadOnly);
    const FBlueprintRenderedSource* const Source = FindLocked(Handle);
    if (Source == nullptr)
    {
        OutText.Reset();
        return false;
    }

    OutText = Source->Text;
    return true;
}

bool FBlueprintSourceCache::TryCopyMap(const int32 Handle, FBlueprintSourceMap& OutMap) const
{
    FRWScopeLock Lock(SourcesLock, SLT_ReadOnly);
    const FBlueprintRenderedSource* const Source = FindLocked(Handle);
    if (Source == nullptr)
    {
        OutMap = FBlueprintSourceMap();
        return false;
    }

    OutMap = Source->Map;
    return true;
}

void FBlueprintSourceCache::Store(const int32 Handle, FBlueprintRenderedSource&& Rendered)
{
    TUniquePtr<FBlueprintRenderedSource> NewSource = MakeUnique<FBlueprintRenderedSource>(MoveTemp(Rendered));
    TUniquePtr<FBlueprintRenderedSource> Retired;
    {
        FRWScopeLock Lock(SourcesLock, SLT_Write);
        TUniquePtr<FBlueprintRenderedSource>& Slot = Sources.FindOrAdd(Handle);
        Retired = MoveTemp(Slot);
        Slot = MoveTemp(NewSource);
    }
}

void FBlueprintSourceCache::Invalidate(const int32 Handle)
{
    TUniquePtr<FBlueprintRenderedSource> Removed;
    {
        FRWScopeLock Lock(SourcesLock, SLT_Write);
        Sources.RemoveAndCopyValue(Handle, Removed);
    }
}

void FBlueprintSourceCache::Clear()
{
    TMap<int32, TUniquePtr<FBlueprintRenderedSource>> Removed;
    {
        FRWScopeLock Lock(SourcesLock, SLT_Write);
        Removed = MoveTemp(Sources);
    }
}

TOptional<FBlueprintRenderedSource> FBlueprintSourceCache::Find(const int32 Handle) const
{
    FRWScopeLock Lock(SourcesLock, SLT_ReadOnly);
    const FBlueprintRenderedSource* const Source = FindLocked(Handle);
    return Source != nullptr ? TOptional<FBlueprintRenderedSource>(*Source) : TOptional<FBlueprintRenderedSource>();
}
