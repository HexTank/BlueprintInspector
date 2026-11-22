#include "BlueprintHotCapture.h"

#include "BlueprintSourceMap.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Misc/App.h"
#include "Misc/DateTime.h"
#include "Misc/EngineVersion.h"
#include "Misc/ScopeLock.h"
#include "UObject/Class.h"

namespace
{
// Response caps: a saved snapshot asks for far more lines than the view polls for, but one response
// still has to be bounded.
constexpr int32 MaxCoverageLinesResponse = 200000;

// A PIE world's package is named UEDPIE_<n>_<map>; a snapshot should record the map, not the instance.
FString StripPIEPrefix(const FString& MapName)
{
    if (!MapName.StartsWith(PiePackagePrefix()))
    {
        return MapName;
    }

    int32 Index = PiePackagePrefixLength;
    while (Index < MapName.Len() && FChar::IsDigit(MapName[Index]))
    {
        ++Index;
    }
    return Index < MapName.Len() && MapName[Index] == TEXT('_') ? MapName.Mid(Index + 1) : MapName;
}

// The map a capture belongs to is the one the game is running, not whichever world the editor is
// editing. Empty when no game world exists yet (a capture can start before PIE).
FString DescribeCaptureMap()
{
    if (GEngine == nullptr)
    {
        return FString();
    }

    FString GameMap;
    for (const FWorldContext& Context : GEngine->GetWorldContexts())
    {
        const UWorld* const World = Context.World();
        if (World == nullptr)
        {
            continue;
        }
        if (Context.WorldType == EWorldType::PIE)
        {
            return StripPIEPrefix(World->GetMapName());
        }
        if (Context.WorldType == EWorldType::Game && GameMap.IsEmpty())
        {
            GameMap = StripPIEPrefix(World->GetMapName());
        }
    }
    return GameMap;
}

FString DescribeBuildConfiguration()
{
    const TCHAR* Configuration = TEXT("Unknown");
    switch (FApp::GetBuildConfiguration())
    {
    case EBuildConfiguration::Debug:
        Configuration = TEXT("Debug");
        break;
    case EBuildConfiguration::DebugGame:
        Configuration = TEXT("DebugGame");
        break;
    case EBuildConfiguration::Development:
        Configuration = TEXT("Development");
        break;
    case EBuildConfiguration::Shipping:
        Configuration = TEXT("Shipping");
        break;
    case EBuildConfiguration::Test:
        Configuration = TEXT("Test");
        break;
    default:
        break;
    }

    // The target type tells a PIE run apart from a packaged one, which two "DebugGame" snapshots
    // otherwise cannot show.
#if WITH_EDITOR
    return FString::Printf(TEXT("%s (Editor)"), Configuration);
#else
    return FString::Printf(TEXT("%s (Game)"), Configuration);
#endif
}
} // namespace

FBlueprintHotCapture::FBlueprintHotCapture(FBlueprintClassRegistry& InRegistry) : Registry(InRegistry)
{
}

FBlueprintHotCaptureStatus FBlueprintHotCapture::Query() const
{
    FBlueprintHotCaptureStatus Status;
    Status.bCapturing = bEnabled.Load();
    Status.Generation = Generation.Load();
    Status.TotalHits = TotalHits.Load();
    return Status;
}

bool FBlueprintHotCapture::TryCopyMetadata(FBlueprintCaptureMetadata& OutMetadata) const
{
    FScopeLock Lock(&MetadataLock);
    const uint32 CurrentGeneration = Generation.Load();
    if (!bMetadataValid || MetadataGeneration != CurrentGeneration)
    {
        return false;
    }
    OutMetadata = MetadataCache;
    return true;
}

FBlueprintCaptureMetadata FBlueprintHotCapture::AcquireMetadata()
{
    // The map and build come from the game thread's world, so this runs there. Warm on start and
    // cached per generation: a snapshot never waits on the world, and a request that arrives after a
    // reset cannot label the new run with the old map.
    const uint32 CurrentGeneration = Generation.Load();
    FBlueprintCaptureMetadata Metadata;
    if (TryCopyMetadata(Metadata))
    {
        return Metadata;
    }

    Metadata.Map = DescribeCaptureMap();
    Metadata.Build = DescribeBuildConfiguration();
    Metadata.Engine = FEngineVersion::Current().ToString(EVersionComponent::Patch);
    Metadata.Project = FApp::GetProjectName();

#if WITH_DEV_AUTOMATION_TESTS
    if (TestDuringMetadataBuild)
    {
        TestDuringMetadataBuild();
    }
#endif

    {
        FScopeLock Lock(&MetadataLock);
        // A Reset/Start during the build makes this metadata describe a run that no longer exists.
        if (Generation.Load() != CurrentGeneration)
        {
            return FBlueprintCaptureMetadata();
        }
        Metadata.CapturedAt = GenerationStartedAt;
        MetadataCache = Metadata;
        MetadataGeneration = CurrentGeneration;
        bMetadataValid = true;
    }
    return Metadata;
}

void FBlueprintHotCapture::BumpGeneration()
{
    FScopeLock Lock(&MetadataLock);
    Generation.Store(Generation.Load() + 1);
    GenerationStartedAt = FDateTime::UtcNow().ToIso8601();
}

FBlueprintHotCaptureStatus FBlueprintHotCapture::Start()
{
    {
        FScopeLock Lock(&HotLock);
        BumpGeneration();
        Hits.Reset();
        TransferHits.Reset();
        TotalHits.Store(0);
        bEnabled.Store(true);
    }

    return Query();
}

FBlueprintHotCaptureStatus FBlueprintHotCapture::Stop()
{
    FScopeLock Lock(&HotLock);
    bEnabled.Store(false);
    return Query();
}

FBlueprintHotCaptureStatus FBlueprintHotCapture::Reset()
{
    {
        FScopeLock Lock(&HotLock);
        BumpGeneration();
        Hits.Reset();
        TransferHits.Reset();
        TotalHits.Store(0);
    }
    return Query();
}

void FBlueprintHotCapture::Clear()
{
    FScopeLock Lock(&HotLock);
    bEnabled.Store(false);
    BumpGeneration();
    Hits.Reset();
    TransferHits.Reset();
    TotalHits.Store(0);
}

void FBlueprintHotCapture::CountStep(const UFunction* const Function, const int64 Address)
{
    if (Function == nullptr || !bEnabled.Load())
    {
        return;
    }

    // A reset can clear the maps and bump the generation between the flag check and the lock below.
    // Re-checking both inside the lock keeps a step from the previous run out of the new one's counters.
    const uint32 StartGeneration = Generation.Load();
    const FBlueprintHotHitKey Key{GetFunctionClassPath(Function), Function->GetFName(), Address};
    {
        FScopeLock Lock(&HotLock);
        if (!bEnabled.Load() || Generation.Load() != StartGeneration)
        {
            return;
        }
        ++Hits.FindOrAdd(Key);
        TotalHits.Store(TotalHits.Load() + 1);
    }
}

void FBlueprintHotCapture::CountTransfer(const UFunction* const Function, const int64 JumpAddress, const int64 Landing)
{
    if (Function == nullptr || !bEnabled.Load())
    {
        return;
    }

    const uint32 StartGeneration = Generation.Load();
    const FBlueprintHotTransferKey Key{GetFunctionClassPath(Function), Function->GetFName(), JumpAddress, Landing};
    {
        FScopeLock Lock(&HotLock);
        if (!bEnabled.Load() || Generation.Load() != StartGeneration)
        {
            return;
        }
        ++TransferHits.FindOrAdd(Key);
        TotalHits.Store(TotalHits.Load() + 1);
    }
}

FBlueprintHotLinesResult FBlueprintHotCapture::AcquireHotLines(const int32 MaxLines) const
{
    FBlueprintHotLinesResult Result;

    TArray<FBlueprintHotHitKey> CounterHits;
    TArray<int64> HitCounts;
    TArray<FBlueprintHotTransferKey> Transfers;
    TArray<int64> TransferCounts;
    {
        FScopeLock Lock(&HotLock);
        Result.Status.bCapturing = bEnabled.Load();
        Result.Status.Generation = Generation.Load();
        Result.Status.TotalHits = TotalHits.Load();
        CounterHits.Reserve(Hits.Num());
        HitCounts.Reserve(Hits.Num());
        for (const TPair<FBlueprintHotHitKey, int64>& Pair : Hits)
        {
            CounterHits.Add(Pair.Key);
            HitCounts.Add(Pair.Value);
        }
        Transfers.Reserve(TransferHits.Num());
        TransferCounts.Reserve(TransferHits.Num());
        for (const TPair<FBlueprintHotTransferKey, int64>& Pair : TransferHits)
        {
            Transfers.Add(Pair.Key);
            TransferCounts.Add(Pair.Value);
        }
    }

    // Only classes with a cached render can be resolved. A capture started before a document is opened
    // shows nothing for that class until the client opens it and asks again. The counters key on the
    // stable class path, so two classes that share a short name stay apart; the display name is only
    // what a response shows.
    TMap<FName, FCachedClassIdentity> Classes;
    Registry.CopyCachedClassIdentities(Classes);

    const auto FindIdentity = [&Classes](const FName ClassPath) -> const FCachedClassIdentity* {
        return Classes.Find(ClassPath);
    };

    TMap<int32, TUniquePtr<FBlueprintSourceMap>> Maps;
    TSet<int32> MissingMaps;
    const auto GetMap = [this, &FindIdentity, &Maps, &MissingMaps](const FName ClassPath) -> const FBlueprintSourceMap* {
        const FCachedClassIdentity* const Identity = FindIdentity(ClassPath);
        if (Identity == nullptr)
        {
            return nullptr;
        }
        if (MissingMaps.Contains(Identity->ID))
        {
            return nullptr;
        }
        if (const TUniquePtr<FBlueprintSourceMap>* const Cached = Maps.Find(Identity->ID))
        {
            return Cached->Get();
        }
        FBlueprintSourceMap Copy;
        if (!Registry.TryCopyMap(Identity->ID, Copy))
        {
            MissingMaps.Add(Identity->ID);
            return nullptr;
        }
        return Maps.Add(Identity->ID, MakeUnique<FBlueprintSourceMap>(MoveTemp(Copy))).Get();
    };

    struct FHotLine
    {
        FString ClassName;
        FString ClassPath;
        FString FunctionName;
        int32 Line = 0;
        int64 Hits = 0;
        bool bTransfer = false;
    };

    TMap<FString, FHotLine> Lines;
    const auto Accumulate = [&Lines](const FName ClassPath, const FString& DisplayName, const FName FunctionName, const int32 Line, const int64 Count, const bool bTransfer) {
        if (Line <= 0)
        {
            return;
        }
        // The path is the merge identity: two classes with the same short name must not share a count.
        const FString Key = FString::Printf(TEXT("%s|%s|%d|%d"), *ClassPath.ToString(), *FunctionName.ToString(), Line, bTransfer ? 1 : 0);
        FHotLine& Entry = Lines.FindOrAdd(Key);
        Entry.ClassName = DisplayName;
        Entry.ClassPath = ClassPath.ToString();
        Entry.FunctionName = FunctionName.ToString();
        Entry.Line = Line;
        Entry.Hits += Count;
        Entry.bTransfer = bTransfer;
    };

    for (int32 Index = 0; Index < CounterHits.Num(); ++Index)
    {
        const FBlueprintHotHitKey& Key = CounterHits[Index];
        const FCachedClassIdentity* const Identity = FindIdentity(Key.ClassPath);
        const FBlueprintSourceMap* const Map = GetMap(Key.ClassPath);
        if (Identity == nullptr || Map == nullptr)
        {
            continue;
        }
        const int32* const Line = Map->LinesByAddressByPath.Find(FBlueprintSourcePathKey{Key.ClassPath, Key.FunctionName, Key.Address});
        if (Line != nullptr)
        {
            Accumulate(Key.ClassPath, Identity->DisplayName, Key.FunctionName, *Line, HitCounts[Index], false);
        }
    }

    for (int32 Index = 0; Index < Transfers.Num(); ++Index)
    {
        const FBlueprintHotTransferKey& Key = Transfers[Index];
        const FCachedClassIdentity* const Identity = FindIdentity(Key.ClassPath);
        const FBlueprintSourceMap* const Map = GetMap(Key.ClassPath);
        if (Identity == nullptr || Map == nullptr)
        {
            continue;
        }
        const int32* const Line = Map->LinesByTransfer.Find(FBlueprintTransferNameKey{Key.ClassPath, Key.FunctionName, Key.JumpAddress, Key.Landing});
        if (Line != nullptr)
        {
            Accumulate(Key.ClassPath, Identity->DisplayName, Key.FunctionName, *Line, TransferCounts[Index], true);
        }
    }

    TArray<FHotLine> Sorted;
    Sorted.Reserve(Lines.Num());
    for (const TPair<FString, FHotLine>& Pair : Lines)
    {
        Sorted.Add(Pair.Value);
    }
    Sorted.Sort([](const FHotLine& A, const FHotLine& B) {
        if (A.Hits != B.Hits)
        {
            return A.Hits > B.Hits;
        }
        if (A.ClassName != B.ClassName)
        {
            return A.ClassName < B.ClassName;
        }
        if (A.ClassPath != B.ClassPath)
        {
            return A.ClassPath < B.ClassPath;
        }
        if (A.FunctionName != B.FunctionName)
        {
            return A.FunctionName < B.FunctionName;
        }
        return A.bTransfer != B.bTransfer ? !A.bTransfer : A.Line < B.Line;
    });

    // A saved snapshot asks for far more lines than the view polls for; the cap only exists to bound
    // one response, not to limit what a capture can record.
    const int32 Limit = FMath::Clamp(MaxLines, 1, MaxHotLinesResponse);
    const int32 Count = FMath::Min(Limit, Sorted.Num());
    Result.bTruncated = Sorted.Num() > Count;
    Result.Lines.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        const FHotLine& Entry = Sorted[Index];
        FBlueprintHotLine& Line = Result.Lines.AddDefaulted_GetRef();
        Line.ClassName = Entry.ClassName;
        Line.FunctionName = Entry.FunctionName;
        Line.Line = Entry.Line;
        Line.Hits = Entry.Hits;
        Line.bTransfer = Entry.bTransfer;
    }

    return Result;
}

FBlueprintCoverageResult FBlueprintHotCapture::AcquireCoverage(const int32 MaxLines) const
{
    FBlueprintCoverageResult Result;

    TArray<FBlueprintHotHitKey> HitKeys;
    TArray<int64> HitCounts;
    TArray<FBlueprintHotTransferKey> TransferKeys;
    TArray<int64> TransferCounts;
    {
        FScopeLock Lock(&HotLock);
        Result.Status.bCapturing = bEnabled.Load();
        Result.Status.Generation = Generation.Load();
        Result.Status.TotalHits = TotalHits.Load();

        HitKeys.Reserve(Hits.Num());
        HitCounts.Reserve(Hits.Num());
        for (const TPair<FBlueprintHotHitKey, int64>& Pair : Hits)
        {
            HitKeys.Add(Pair.Key);
            HitCounts.Add(Pair.Value);
        }

        TransferKeys.Reserve(TransferHits.Num());
        TransferCounts.Reserve(TransferHits.Num());
        for (const TPair<FBlueprintHotTransferKey, int64>& Pair : TransferHits)
        {
            TransferKeys.Add(Pair.Key);
            TransferCounts.Add(Pair.Value);
        }
    }

    TMap<FBlueprintHotHitKey, int64> HitsCopy;
    HitsCopy.Reserve(HitKeys.Num());
    for (int32 Index = 0; Index < HitKeys.Num(); ++Index)
    {
        HitsCopy.Add(HitKeys[Index], HitCounts[Index]);
    }

    TMap<FBlueprintHotTransferKey, int64> TransfersCopy;
    TransfersCopy.Reserve(TransferKeys.Num());
    for (int32 Index = 0; Index < TransferKeys.Num(); ++Index)
    {
        TransfersCopy.Add(TransferKeys[Index], TransferCounts[Index]);
    }

    // Only classes with a cached render can be resolved, as with hot paths: a class the client never
    // opened has no lines to report, so it is counted neither as covered nor as uncovered. The counters
    // key on the class path; the response shows the display name.
    TMap<FName, FCachedClassIdentity> Classes;
    Registry.CopyCachedClassIdentities(Classes);

    TArray<FBlueprintCoverageLine> All;
    for (const TPair<FName, FCachedClassIdentity>& ClassPair : Classes)
    {
        const FName ClassPath = ClassPair.Key;
        const FCachedClassIdentity& Class = ClassPair.Value;
        FBlueprintSourceMap Map;
        if (!Registry.TryCopyMap(Class.ID, Map))
        {
            continue;
        }
        ++Result.RenderedClasses;

        for (int32 Index = 0; Index < Map.Lines.Num(); ++Index)
        {
            const FBlueprintLineInfo& Info = Map.Lines[Index];
            const UFunction* const Function = Info.OwnerFunction.Get();
            if (Function == nullptr || !Info.IsBreakpointable())
            {
                continue;
            }

            // A statement owns one address, and a break/continue arm owns one jump/landing pair. The
            // remaining kinds (sequence boundary, goto, sub-expression) share the statement's address,
            // so counting them would report the same execution twice.
            const bool bTransfer = Info.Kind == EBlueprintLineKind::LoopTransfer;
            if (!bTransfer && Info.Kind != EBlueprintLineKind::Statement)
            {
                continue;
            }

            const FName FunctionName = Function->GetFName();
            int64 Count = 0;
            if (bTransfer)
            {
                if (const int64* const Found = TransfersCopy.Find(FBlueprintHotTransferKey{
                        ClassPath, FunctionName, Info.ScriptIndex, Info.TransferTarget}))
                {
                    Count = *Found;
                }
            }
            else if (const int64* const Found = HitsCopy.Find(FBlueprintHotHitKey{ClassPath, FunctionName, Info.ScriptIndex}))
            {
                Count = *Found;
            }

            FBlueprintCoverageLine& Line = All.AddDefaulted_GetRef();
            Line.ClassName = Class.DisplayName;
            Line.ClassPath = ClassPath.ToString();
            Line.FunctionName = FunctionName.ToString();
            Line.Line = Index + 1;
            Line.Hits = Count;
            Line.bTransfer = bTransfer;
        }
    }

    // Class/function/line order is what both exports want and what makes the view stable between
    // requests; the truncation is applied after the sort so a limited response is still a prefix.
    All.Sort([](const FBlueprintCoverageLine& A, const FBlueprintCoverageLine& B) {
        const int32 ByClass = A.ClassName.Compare(B.ClassName);
        if (ByClass != 0)
        {
            return ByClass < 0;
        }
        const int32 ByClassPath = A.ClassPath.Compare(B.ClassPath);
        if (ByClassPath != 0)
        {
            return ByClassPath < 0;
        }
        const int32 ByFunction = A.FunctionName.Compare(B.FunctionName);
        if (ByFunction != 0)
        {
            return ByFunction < 0;
        }
        if (A.Line != B.Line)
        {
            return A.Line < B.Line;
        }
        return !A.bTransfer && B.bTransfer;
    });

    Result.TotalLines = All.Num();
    for (const FBlueprintCoverageLine& Line : All)
    {
        if (Line.Hits > 0)
        {
            ++Result.CoveredLines;
        }
        else
        {
            ++Result.UncoveredLines;
        }
    }

    const int32 Limit = FMath::Clamp(MaxLines, 0, MaxCoverageLinesResponse);
    const int32 Count = FMath::Min(Limit, All.Num());
    Result.bTruncated = Count < All.Num();
    Result.Lines.Reserve(Count);
    for (int32 Index = 0; Index < Count; ++Index)
    {
        Result.Lines.Add(MoveTemp(All[Index]));
    }

    return Result;
}
