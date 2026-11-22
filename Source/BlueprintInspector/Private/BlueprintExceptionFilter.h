#pragma once

#include "CoreMinimal.h"

// Which Blueprint runtime errors stop the game. A bit mask, so the DAP thread can replace the whole set
// with one atomic store; an unselected error is left to the engine's own reporting.
enum class EBlueprintExceptionFilter : int32
{
    None = 0,
    // AccessViolation: null access, a missing property, an out-of-range array get.
    AccessedNone = 1 << 0,
    // NonFatalError and FatalError (and any newer error kind): the errors the VM logs and runs past.
    RuntimeError = 1 << 1,
    // The VM's runaway-loop guard.
    InfiniteLoop = 1 << 2,
};

static_assert(sizeof(EBlueprintExceptionFilter) == sizeof(int32), "The exception filter must share the mask width.");

// The wire names are the source of truth for both the parser and the capabilities response.
struct FExceptionFilterName
{
    EBlueprintExceptionFilter Filter;
    const ANSICHAR* Name;
};

inline const TArray<FExceptionFilterName>& GetExceptionFilterNames()
{
    static const TArray<FExceptionFilterName> Names = {
        {EBlueprintExceptionFilter::AccessedNone, "accessedNone"},
        {EBlueprintExceptionFilter::RuntimeError, "runtimeError"},
        {EBlueprintExceptionFilter::InfiniteLoop, "infiniteLoop"},
    };
    return Names;
}

inline const ANSICHAR* GetExceptionFilterName(const EBlueprintExceptionFilter Filter)
{
    for (const FExceptionFilterName& Entry : GetExceptionFilterNames())
    {
        if (Entry.Filter == Filter)
        {
            return Entry.Name;
        }
    }
    return "";
}

// The set of Blueprint runtime errors that stop the game; the predicates keep call sites from doing
// bit arithmetic.
class FExceptionFilterSet
{
public:
    void Add(const EBlueprintExceptionFilter Filter)
    {
        check(Filter != EBlueprintExceptionFilter::None);
        Mask |= static_cast<int32>(Filter);
    }

    [[nodiscard]] bool Contains(const EBlueprintExceptionFilter Filter) const
    {
        return (Mask & static_cast<int32>(Filter)) != 0;
    }

    [[nodiscard]] bool IsEmpty() const { return Mask == 0; }
    [[nodiscard]] int32 ToMask() const { return Mask; }

    static FExceptionFilterSet FromMask(const int32 InMask)
    {
        FExceptionFilterSet Set;
        for (const FExceptionFilterName& Entry : GetExceptionFilterNames())
        {
            if ((InMask & static_cast<int32>(Entry.Filter)) != 0)
            {
                Set.Mask |= static_cast<int32>(Entry.Filter);
            }
        }
        return Set;
    }

    // Adds the filter a DAP client named. False when this build does not know the name.
    bool AddByName(const FString& InName)
    {
        const FString Name = InName.TrimStartAndEnd();
        for (const FExceptionFilterName& Entry : GetExceptionFilterNames())
        {
            if (Name.Equals(ANSI_TO_TCHAR(Entry.Name), ESearchCase::IgnoreCase))
            {
                Add(Entry.Filter);
                return true;
            }
        }
        return false;
    }

private:
    int32 Mask = 0;
};
