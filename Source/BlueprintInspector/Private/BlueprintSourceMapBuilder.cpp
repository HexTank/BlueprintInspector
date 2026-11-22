#include "BlueprintSourceMapBuilder.h"
#include "BlueprintInspector.h"
#include "BlueprintInspectorPort.h"

namespace
{
// A first guess for the line array: one line per this many bytes, with a small floor.
constexpr int32 MinLineReserve = 16;
constexpr int32 BytesPerEstimatedLine = 24;

// No single function's render has this many location records; a caller that asks for more is counted from
// a corrupt script array, and reserving it would be a multi-gigabyte allocation.
constexpr int32 MaxLocationReserve = 256 * 1024;
} // namespace

void FBlueprintSourceMapBuilder::Reset()
{
    check(IsInGameThread());
    Locations.Reset();
    bLoggedNegativeOffset = false;
    bLoggedOutOfOrderOffset = false;
}

void FBlueprintSourceMapBuilder::Reserve(const int32 Count)
{
    check(IsInGameThread());

    // A negative count would silently do nothing and hide a counting bug; a huge one would attempt a
    // large allocation from an API callers assume is safe.
    checkf(Count >= 0, TEXT("FBlueprintSourceMapBuilder::Reserve requires a non-negative count"));
    Locations.Reserve(FMath::Clamp(Count, 0, MaxLocationReserve));
}

void FBlueprintSourceMapBuilder::AddLocation(const int32 Offset, const FBlueprintLineInfo& Info)
{
    check(IsInGameThread());

    // Build() advances a single forward cursor, so out-of-order or negative offsets would silently
    // mis-attribute every later line. Drop the record and report it instead of aborting the editor (and
    // instead of letting the same input corrupt the map in a Shipping build, where checkf is compiled out).
    if (Offset < 0)
    {
        if (!bLoggedNegativeOffset)
        {
            bLoggedNegativeOffset = true;
            UE_LOG(LogBlueprintInspector, Error, TEXT("Source map: ignoring a negative offset (%d)"), Offset);
        }
        return;
    }
    if (Locations.Num() > 0 && Offset < Locations.Last().Offset)
    {
        if (!bLoggedOutOfOrderOffset)
        {
            bLoggedOutOfOrderOffset = true;
            UE_LOG(LogBlueprintInspector, Error, TEXT("Source map: ignoring offset %d after %d"), Offset, Locations.Last().Offset);
        }
        return;
    }
    Locations.Add(FLocation{Offset, Info});
}

void FBlueprintSourceMapBuilder::TrimFrom(const int32 Offset)
{
    check(IsInGameThread());
    if (Offset < 0)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Source map: ignoring a negative trim offset (%d)"), Offset);
        return;
    }

    // Records are appended in nondecreasing offset order, so the trimmed tail is a suffix of the array.
    while (Locations.Num() > 0 && Locations.Last().Offset >= Offset)
    {
        Locations.Pop(BI_POP_NO_SHRINK);
    }
}

FBlueprintSourceMap FBlueprintSourceMapBuilder::Build(const FString& InText)
{
    check(IsInGameThread());

    FBlueprintSourceMap Map;
    Map.Lines.Reserve(FMath::Max(MinLineReserve, InText.Len() / BytesPerEstimatedLine));

    int32 LocationIndex = 0;
    const int32 Length = InText.Len();

    for (int32 LineStart = 0; LineStart < Length;)
    {
        while (LocationIndex + 1 < Locations.Num() && Locations[LocationIndex + 1].Offset <= LineStart)
        {
            ++LocationIndex;
        }

        const bool bCovered = Locations.IsValidIndex(LocationIndex) && Locations[LocationIndex].Offset <= LineStart;
        FBlueprintLineInfo Info = bCovered ? Locations[LocationIndex].Info : FBlueprintLineInfo{};

        // A line after the statement's own belongs to the same address, but is not the line that address
        // should be reported against.
        if (bCovered && Info.Kind == EBlueprintLineKind::Statement && LineStart > Locations[LocationIndex].Offset)
        {
            Info.Kind = EBlueprintLineKind::SubStatement;
        }
        // A sequence boundary, goto marker or break/continue transfer is a one-line record. A line that
        // inherits it without a new location is not part of it, so it must not carry its address or arm.
        else if (bCovered && (Info.Kind == EBlueprintLineKind::Sequence || Info.Kind == EBlueprintLineKind::Goto || Info.Kind == EBlueprintLineKind::LoopTransfer) && LineStart > Locations[LocationIndex].Offset)
        {
            Info.Kind = EBlueprintLineKind::None;
            // The record only covers its own line: the address and arm go, while the owner stays and
            // IsBreakpointable() rejects the line on Kind.
            Info.ScriptIndex = INDEX_NONE;
            Info.TransferTarget = INDEX_NONE;
        }
        Map.Lines.Add(MoveTemp(Info));

        int32 LineEnd = LineStart;
        while (LineEnd < Length && InText[LineEnd] != TEXT('\n'))
        {
            ++LineEnd;
        }
        LineStart = LineEnd + 1;
    }

    Map.BuildReverseIndex();

#if DO_CHECK
    for (int32 Index = 0; Index < Locations.Num(); ++Index)
    {
        if (Locations[Index].Offset >= Length)
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Source map: %d location record(s) past the end of the rendered text (first offset %d, length %d)"), Locations.Num() - Index, Locations[Index].Offset, Length);
            break;
        }
    }
#endif

    Locations.Reset();
    bLoggedNegativeOffset = false;
    bLoggedOutOfOrderOffset = false;

    return Map;
}
