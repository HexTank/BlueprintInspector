#pragma once

#include "CoreMinimal.h"
#include "BlueprintKeys.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "UObject/Class.h"

enum class EBlueprintLineKind : uint8
{
    // A line that carries no breakpoint: a brace, an `else`, the signature, a `goto` marker.
    None,
    Statement,
    // An `EX_PushExecutionFlow` sequence boundary (`// Sequence N.`). The address is the first bytecode of
    // the pin that follows, so the line arms the same address as that pin's first statement and the reverse
    // lookup prefers the Statement. An empty pin has no statement, so its boundary is the only line for the
    // address.
    Sequence,
    // A `goto _0x…;` transfer to a statement an earlier edge already drew. The address is that statement's,
    // so the reverse lookup prefers the Statement; a transfer whose target has no line owns the address.
    Goto,
    // A line after the statement's own, emitted from inside an expression the renderer inlined with newlines
    // already in it (the EX_SwitchValue lambda). Breakpointable, but not the line the address is reported
    // against, so the reverse lookup prefers the Statement.
    SubStatement,
    // A `break;`/`continue;` arm. ScriptIndex is the arm's jump opcode and TransferTarget is the offset the
    // VM holds once that handler has run. The jump is dispatched for both arms, so the opcode alone cannot
    // identify the arm: the line arms this pair and stops after dispatch, and owns no reverse entry.
    LoopTransfer,
};

/**
 * Where one rendered line sits in bytecode. OwnerFunction is required: bytecode offsets are
 * function-local, and one rendered file spans several UFunction::Script arrays.
 */
struct FBlueprintLineInfo
{
    TWeakObjectPtr<const UFunction> OwnerFunction;
    int64 ScriptIndex = INDEX_NONE;
    EBlueprintLineKind Kind = EBlueprintLineKind::None;

    // LoopTransfer only: the offset the VM holds after the arm's jump handler ran. Together with ScriptIndex
    // it is the dispatch the stop waits for.
    int64 TransferTarget = INDEX_NONE;

    [[nodiscard]] bool IsBreakpointable() const
    {
        const bool bHasAddress = Kind != EBlueprintLineKind::LoopTransfer || TransferTarget >= 0;
        return Kind != EBlueprintLineKind::None && ScriptIndex >= 0 && OwnerFunction.IsValid() && bHasAddress;
    }
};

// A stable class identity that survives PIE duplication: the object path with the PIE package prefix
// removed. The editor class and its PIE copy share it; same-named classes in other packages do not.
//
// The capture hooks call this for every script step, and building the path allocates an FString, so the
// result is remembered per class. Weak keys stop a collected class from being confused with a new class
// that lands at the same address. Callers are on the game thread today; the lock exists because the
// cache is process-wide and that is not something this helper can enforce.
// A PIE duplicate carries the prefix on both the package and the object name
// (/Game/BP/UEDPIE_0_Foo.UEDPIE_0_Foo_C). Strip every well-formed occurrence so the key matches the
// editor path; a UEDPIE_ that is not followed by digits and an underscore is left alone.
inline void StripPiePackagePrefixesInline(FString& Path)
{
    int32 SearchFrom = 0;
    for (int32 PiePrefix = Path.Find(PiePackagePrefix(), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom); PiePrefix != INDEX_NONE;
         PiePrefix = Path.Find(PiePackagePrefix(), ESearchCase::CaseSensitive, ESearchDir::FromStart, SearchFrom))
    {
        int32 End = PiePrefix + PiePackagePrefixLength;
        while (End < Path.Len() && FChar::IsDigit(Path[End]))
        {
            ++End;
        }
        if (End == PiePrefix + PiePackagePrefixLength || End >= Path.Len() || Path[End] != TEXT('_'))
        {
            SearchFrom = PiePrefix + 1;
            continue;
        }
        Path.RemoveAt(PiePrefix, End - PiePrefix + 1);
        SearchFrom = PiePrefix;
    }
}

inline FName GetClassPath(const UClass* const Class)
{
    if (Class == nullptr)
    {
        return NAME_None;
    }

    struct FClassPathCache
    {
        FCriticalSection Lock;
        TMap<TWeakObjectPtr<const UClass>, FName> Paths;
    };

    // A long session meets thousands of classes; dropping the cache wholesale past this is cheaper than
    // tracking which entries are still worth keeping.
    constexpr int32 MaxCachedClassPaths = 4096;

    // Deliberately never destroyed: the cache outlives every object that uses it, so a shutdown-order
    // dependency would be the only thing a destructor could add.
    static FClassPathCache* const Cache = new FClassPathCache();

    {
        FScopeLock Guard(&Cache->Lock);
        if (const FName* const Found = Cache->Paths.Find(Class))
        {
            return *Found;
        }
    }

    FString Path = Class->GetPathName();
    StripPiePackagePrefixesInline(Path);
    const FName Result(*Path);

    {
        FScopeLock Guard(&Cache->Lock);
        if (Cache->Paths.Num() >= MaxCachedClassPaths)
        {
            for (auto It = Cache->Paths.CreateIterator(); It; ++It)
            {
                if (!It.Key().IsValid())
                {
                    It.RemoveCurrent();
                }
            }
            if (Cache->Paths.Num() >= MaxCachedClassPaths)
            {
                Cache->Paths.Reset();
            }
        }
        Cache->Paths.Add(Class, Result);
    }
    return Result;
}

inline FName GetFunctionClassPath(const UFunction* const Function)
{
    return GetClassPath(Function != nullptr ? Function->GetOwnerClass() : nullptr);
}

// Weak, because the reverse index outlives a single frame and may be read after an editor reload has
// collected the function the map was built from.
struct FBlueprintSourceKey
{
    TWeakObjectPtr<const UFunction> Function;
    int64 ScriptIndex = INDEX_NONE;

    bool operator==(const FBlueprintSourceKey& Other) const
    {
        return Function == Other.Function && ScriptIndex == Other.ScriptIndex;
    }

    friend uint32 GetTypeHash(const FBlueprintSourceKey& Key)
    {
        // Hash the weak identity (object index/serial), not the resolved pointer: Get() turns null once
        // the function is collected, which would change the key's hash and make the element unreachable
        // in its bucket (TMap requires a key's hash to stay constant).
        return HashCombine(GetTypeHash(Key.Function), GetTypeHash(Key.ScriptIndex));
    }
};

/**
 * The breakpoint map for one rendered source. Lines has one entry per line; DAP line N is Lines[N - 1].
 * LinesByAddress is the reverse relation, keyed by the bytecode a line reports. A line is the canonical
 * reverse entry; a SubStatement shares the key only when it has no Statement to represent it.
 */
struct FBlueprintSourceMap
{
    TArray<FBlueprintLineInfo> Lines;
    TMap<FBlueprintSourceKey, int32> LinesByAddress;
    // PIE-duplicate lookup: the same lines keyed by class path, function and address.
    TMap<FBlueprintSourcePathKey, int32> LinesByAddressByPath;
    TMap<FBlueprintTransferNameKey, int32> LinesByTransfer;

    /**
     * Whether a DAP line can carry a breakpoint: it has an entry, and that entry has an address behind it.
     * The DAP line is 1-based and Lines is 0-based, so line N is Lines[N - 1] -- this is the one place that
     * conversion is written down, and an out-of-range line is simply not breakpointable.
     */
    [[nodiscard]] bool IsBreakpointable(const int32 Line) const
    {
        return Line > 0 && Lines.IsValidIndex(Line - 1) && Lines[Line - 1].IsBreakpointable();
    }

    /**
     * The 1-based line a bytecode address is reported against, or 0 when no rendered line owns it. When
     * several lines share the address, the Statement line wins over a SubStatement.
     */
    [[nodiscard]] int32 FindLine(const UFunction* const Function, const int64 Address) const
    {
        if (Function == nullptr)
        {
            return 0;
        }

        if (const int32* const Line = LinesByAddress.Find(FBlueprintSourceKey{Function, Address}))
        {
            return *Line;
        }

        const int32* const PathLine = LinesByAddressByPath.Find(FBlueprintSourcePathKey{GetFunctionClassPath(Function), Function->GetFName(), Address});
        return PathLine != nullptr ? *PathLine : 0;
    }

    /**
     * The 1-based line a suspended caller frame reports: the nearest Statement at or before Offset
     * inside Function. A caller's Code points past the call opcode and its operands, so the exact
     * address of the call is behind it. The function-name fallback matches FindLine, for PIE duplicates.
     */
    [[nodiscard]] int32 FindLineAtOrBefore(const UFunction* const Function, const int64 Offset) const
    {
        if (Function == nullptr || Offset < 0)
        {
            return 0;
        }

        int32 BestLine = 0;
        int64 BestAddress = INDEX_NONE;

        const FName FunctionName = Function->GetFName();
        FName FunctionClassPath;
        bool bFunctionClassPathResolved = false;
        TMap<const UFunction*, bool> OwnerMatches;

        for (int32 Index = 0; Index < Lines.Num(); ++Index)
        {
            const FBlueprintLineInfo& Info = Lines[Index];
            if (Info.Kind != EBlueprintLineKind::Statement || Info.ScriptIndex < 0 || Info.ScriptIndex > Offset)
            {
                continue;
            }

            const UFunction* const Owner = Info.OwnerFunction.Get();
            if (Owner == nullptr)
            {
                continue;
            }

            bool bSameSignature = Owner == Function;
            if (!bSameSignature && Owner->GetFName() == FunctionName)
            {
                if (const bool* const Cached = OwnerMatches.Find(Owner))
                {
                    bSameSignature = *Cached;
                }
                else
                {
                    if (!bFunctionClassPathResolved)
                    {
                        FunctionClassPath = GetFunctionClassPath(Function);
                        bFunctionClassPathResolved = true;
                    }
                    bSameSignature = FunctionClassPath != NAME_None && GetFunctionClassPath(Owner) == FunctionClassPath;
                    OwnerMatches.Add(Owner, bSameSignature);
                }
            }
            if (!bSameSignature)
            {
                continue;
            }

            if (BestLine == 0 || Info.ScriptIndex > BestAddress)
            {
                BestLine = Index + 1;
                BestAddress = Info.ScriptIndex;
            }
        }

        return BestLine;
    }

    void BuildReverseIndex()
    {
        LinesByAddress.Reset();
        LinesByAddressByPath.Reset();
        LinesByTransfer.Reset();
        LinesByAddress.Reserve(Lines.Num());
        LinesByAddressByPath.Reserve(Lines.Num());

        const UFunction* LastFunction = nullptr;
        FName LastClassPath;

        for (int32 Index = 0; Index < Lines.Num(); ++Index)
        {
            const FBlueprintLineInfo& Info = Lines[Index];
            const UFunction* const Function = Info.OwnerFunction.Get();
            if (!Info.IsBreakpointable() || Function == nullptr)
            {
                continue;
            }

            if (Function != LastFunction)
            {
                LastFunction = Function;
                LastClassPath = GetFunctionClassPath(Function);
            }

            // A transfer line reports no address: its jump opcode belongs to the statement line that owns it
            // (the `if` above it), and the reverse lookup must keep handing that address to the statement.
            if (Info.Kind == EBlueprintLineKind::LoopTransfer)
            {
                if (Info.TransferTarget >= 0)
                {
                    LinesByTransfer.FindOrAdd(FBlueprintTransferNameKey{LastClassPath, Function->GetFName(), Info.ScriptIndex, Info.TransferTarget}, Index + 1);
                }
                continue;
            }

            int32& Line = LinesByAddress.FindOrAdd(FBlueprintSourceKey{Function, Info.ScriptIndex});
            if (Line == 0 || (Info.Kind == EBlueprintLineKind::Statement && Lines[Line - 1].Kind != EBlueprintLineKind::Statement))
            {
                Line = Index + 1;
            }

            if (LastClassPath != NAME_None)
            {
                const FBlueprintSourcePathKey PathKey{LastClassPath, Function->GetFName(), Info.ScriptIndex};
                int32& PathLine = LinesByAddressByPath.FindOrAdd(PathKey);
                if (PathLine == 0 || (Info.Kind == EBlueprintLineKind::Statement && Lines[PathLine - 1].Kind != EBlueprintLineKind::Statement))
                {
                    PathLine = Index + 1;
                }
            }
        }
    }
};

/**
 * The line a DAP request should arm. A non-breakpointable line snaps forward to the next line with an
 * address, but never across a function boundary: the nearest preceding owner decides the section, so a
 * brace in one function cannot arm a breakpoint in the next one.
 */
inline int32 FindBreakpointLine(const TArray<FBlueprintLineInfo>& Lines, const int32 RequestedLine)
{
    if (Lines.Num() == 0 || RequestedLine < 1)
    {
        return 0;
    }

    const UFunction* SectionFunction = nullptr;
    for (int32 Line = FMath::Min(RequestedLine, Lines.Num()); Line >= 1; --Line)
    {
        if (const UFunction* const Owner = Lines[Line - 1].OwnerFunction.Get())
        {
            SectionFunction = Owner;
            break;
        }
    }

    if (SectionFunction == nullptr)
    {
        // No owner at or before the request (a header line, or a collected function): arming any later
        // line would snap across a function boundary, which this function forbids.
        return 0;
    }

    for (int32 Line = FMath::Max(1, RequestedLine); Line <= Lines.Num(); ++Line)
    {
        const FBlueprintLineInfo& Info = Lines[Line - 1];
        if (!Info.IsBreakpointable())
        {
            continue;
        }

        if (Info.OwnerFunction.Get() != SectionFunction)
        {
            return 0;
        }

        return Line;
    }

    return 0;
}

inline int32 CountRenderedLines(const FString& InText)
{
    const int32 Length = InText.Len();
    if (Length == 0)
    {
        return 0;
    }

    int32 LineCount = 0;
    for (int32 Index = 0; Index < Length; ++Index)
    {
        LineCount += InText[Index] == TEXT('\n') ? 1 : 0;
    }

    return InText[Length - 1] == TEXT('\n') ? LineCount : LineCount + 1;
}

// The rendered signature for one ubergraph entry: the offset is inserted before the parenthesis, so
// `ExecuteUbergraph_X(int32 EntryPoint)` becomes `ExecuteUbergraph_X_1403(int32 EntryPoint)` and two
// entry points stop sharing one name.
inline FString UbergraphEntrySignature(const FString& Signature, const int64 Entry)
{
    FString Result = Signature;
    const int32 Paren = Result.Find(TEXT("("));
    if (Paren != INDEX_NONE)
    {
        Result.InsertAt(Paren, FString::Printf(TEXT("_%lld"), Entry));
    }
    else
    {
        Result += FString::Printf(TEXT("_%lld"), Entry);
    }
    return Result;
}

struct FBlueprintRenderedSource
{
    FString Text;
    FBlueprintSourceMap Map;
};
