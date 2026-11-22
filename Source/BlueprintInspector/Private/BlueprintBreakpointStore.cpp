#include "BlueprintBreakpointStore.h"

#include "BlueprintInspector.h"
#include "BlueprintInspectorDetours.h"
#include "UObject/Class.h"

namespace
{
bool MakeBreakpointKey(const UFunction* const Function, const int64 Address, FBreakpointKey& Out)
{
    const UClass* const OwnerClass = Function ? Function->GetOwnerClass() : nullptr;
    if (OwnerClass == nullptr)
    {
        return false;
    }

    Out.ClassPath = GetFunctionClassPath(Function);
    Out.FunctionName = Function->GetFName();
    Out.Address = Address;
    return true;
}

bool MakeBreakpointKey(const TWeakObjectPtr<const UFunction>& Function, const int64 Address, FBreakpointKey& Out)
{
    return MakeBreakpointKey(Function.Get(), Address, Out);
}

bool MakeBreakpointTransferKey(const UFunction* const Function, const int64 JumpAddress, const int64 Landing, FBreakpointTransferKey& Out)
{
    const UClass* const OwnerClass = Function ? Function->GetOwnerClass() : nullptr;
    if (OwnerClass == nullptr)
    {
        return false;
    }

    Out.ClassPath = GetFunctionClassPath(Function);
    Out.FunctionName = Function->GetFName();
    Out.JumpAddress = JumpAddress;
    Out.Landing = Landing;
    return true;
}

// A setBreakpoints request resolved against lines that are already rendered. Pure: the caller passes
// the lines, or null when the source is not cached, so this never renders or reads state.
TArray<FBlueprintBreakpointResolution> ResolveBreakpointLines(const TArray<FBlueprintLineInfo>* const Lines, const TArray<FBlueprintBreakpointRequest>& Requests)
{
    TArray<FBlueprintBreakpointResolution> Resolutions;
    Resolutions.Reserve(Requests.Num());

    for (const FBlueprintBreakpointRequest& Request : Requests)
    {
        FBlueprintBreakpointResolution Resolution;
        Resolution.RequestedLine = Request.Line;
        Resolution.bSourceAvailable = Lines != nullptr;

        if (Lines)
        {
            Resolution.Line = FindBreakpointLine(*Lines, Request.Line);
        }

        Resolutions.Add(Resolution);
    }

    return Resolutions;
}
} // namespace

FBlueprintBreakpointStore::FBlueprintBreakpointStore(FBlueprintClassRegistry& InRegistry) : Registry(InRegistry)
{
}

FBreakpointRecord* FBlueprintBreakpointStore::FindRecord(const UFunction* const Function, const int64 Address, FBreakpointKey& OutKey)
{
    check(IsInGameThread());
    return MakeBreakpointKey(Function, Address, OutKey) ? Records.Find(OutKey) : nullptr;
}

bool FBlueprintBreakpointStore::IsSet(const UFunction* const InFunction, const int64 InAddress) const
{
    check(IsInGameThread());

    if (InFunction == nullptr)
    {
        return false;
    }

    const TWeakObjectPtr<const UFunction> FunctionKey(InFunction);
    if (const TArray<int64>* const Entry = Addresses.Find(FunctionKey))
    {
        if (Entry->Contains(InAddress))
        {
            return true;
        }
    }

    FBreakpointKey Key;
    if (!MakeBreakpointKey(InFunction, InAddress, Key))
    {
        return false;
    }

    const FBreakpointRecord* const Record = Records.Find(Key);
    return Record != nullptr && Record->Function.IsValid();
}

bool FBlueprintBreakpointStore::IsArmed(const FBreakpointKey& Key) const
{
    const FBreakpointRecord* const Record = Records.Find(Key);
    return Record != nullptr && Record->Function.IsValid();
}

EBreakpointToggle FBlueprintBreakpointStore::Toggle(const TWeakObjectPtr<const UFunction>& InFunction, const int64 InAddress)
{
    check(IsInGameThread());

    FBreakpointKey Key;
    if (!InFunction.IsValid() || !MakeBreakpointKey(InFunction, InAddress, Key))
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("breakpoint toggle rejected: the function is not a valid breakpoint owner"));
        return EBreakpointToggle::Rejected;
    }

    const EBreakpointToggle Result = ToggleNoIndex(InFunction, InAddress);
    RebuildIndex();

    switch (Result)
    {
    case EBreakpointToggle::Set:
        UE_LOG(LogBlueprintInspector, Log, TEXT("breakpoint set at %s.%s@%lld"), *Key.ClassPath.ToString(), *Key.FunctionName.ToString(), Key.Address);
        break;
    case EBreakpointToggle::Cleared:
        UE_LOG(LogBlueprintInspector, Log, TEXT("breakpoint cleared at %s.%s@%lld"), *Key.ClassPath.ToString(), *Key.FunctionName.ToString(), Key.Address);
        break;
    case EBreakpointToggle::Rejected:
        UE_LOG(LogBlueprintInspector, Warning, TEXT("breakpoint toggle rejected at %s.%s@%lld: the address is not breakpointable"), *Key.ClassPath.ToString(), *Key.FunctionName.ToString(), Key.Address);
        break;
    }
    return Result;
}

EBreakpointToggle FBlueprintBreakpointStore::ToggleNoIndex(const TWeakObjectPtr<const UFunction>& InFunction, const int64 InAddress)
{
    check(IsInGameThread());

    if (!InFunction.IsValid() || InAddress < 0 || InFunction->Script.Num() <= InAddress || InFunction->HasAnyFunctionFlags(FUNC_Native))
    {
        return EBreakpointToggle::Rejected;
    }

    FBreakpointKey Key;
    if (!MakeBreakpointKey(InFunction, InAddress, Key))
    {
        return EBreakpointToggle::Rejected;
    }

    bool bWasSet = false;
    if (const FBreakpointRecord* const Existing = Records.Find(Key))
    {
        // A record whose function was collected is not armed: it must not read as "set" and then be
        // removed by this call, which would report "cleared" for a function where nothing is armed.
        bWasSet = Existing->Function.IsValid();
        Records.Remove(Key);
    }
    if (!bWasSet)
    {
        Records.Add(Key, FBreakpointRecord{InFunction});
    }
    return bWasSet ? EBreakpointToggle::Cleared : EBreakpointToggle::Set;
}

bool FBlueprintBreakpointStore::ContainsTransfer(const UFunction* const Function, const int64 JumpAddress, const int64 Landing) const
{
    check(IsInGameThread());
    FBreakpointTransferKey Key;
    return MakeBreakpointTransferKey(Function, JumpAddress, Landing, Key) && Transfers.Contains(Key);
}

void FBlueprintBreakpointStore::RebuildIndex()
{
    check(IsInGameThread());

    Addresses.Reset();
    for (auto It = Records.CreateIterator(); It; ++It)
    {
        // A collected function cannot be dispatched again, so its record is unreachable. Drop it here
        // rather than leaving a key that IsSet() would report as set and Toggle() would read as armed.
        if (!It.Value().Function.IsValid())
        {
            It.RemoveCurrent();
            continue;
        }

        const UFunction* const Function = It.Value().Function.Get();
        TArray<int64>& FunctionAddresses = Addresses.FindOrAdd(Function);
        FunctionAddresses.Add(It.Key().Address);
    }
}

void FBlueprintBreakpointStore::RebindClass(const UClass* const Class)
{
    check(IsInGameThread());
    if (Class)
    {
        const FName ClassPath = GetClassPath(Class);
        for (auto It = Records.CreateIterator(); It; ++It)
        {
            if (It.Key().ClassPath != ClassPath)
            {
                continue;
            }

            const UFunction* const NewFunction = Class->FindFunctionByName(It.Key().FunctionName);
            // The cache key survives a retarget, but the address is only meaningful inside the new
            // function's script. Drop records that no longer name a byte instead of leaving a
            // breakpoint that can never fire.
            if (NewFunction == nullptr || NewFunction->GetOwnerClass() != Class || It.Key().Address < 0 || It.Key().Address >= NewFunction->Script.Num())
            {
                UE_LOG(LogBlueprintInspector, Warning, TEXT("Dropping breakpoint %s.%s@%lld: it does not address the retargeted class"), *It.Key().ClassPath.ToString(), *It.Key().FunctionName.ToString(), It.Key().Address);
                It.RemoveCurrent();
                continue;
            }

            It.Value().Function = NewFunction;
        }

        // A transfer key names a jump opcode and a landing inside one script, so a retargeted class has to
        // keep both. A pair that no longer fits is dropped rather than left armed forever.
        for (auto It = Transfers.CreateIterator(); It; ++It)
        {
            if (It->ClassPath != ClassPath)
            {
                continue;
            }

            const UFunction* const NewFunction = Class->FindFunctionByName(It->FunctionName);
            // A landing may legitimately be the end of the script (a jump past the last opcode), so only an
            // offset beyond it is stale.
            if (NewFunction == nullptr || NewFunction->GetOwnerClass() != Class || It->JumpAddress < 0 || It->JumpAddress >= NewFunction->Script.Num() || It->Landing < 0 || It->Landing > NewFunction->Script.Num())
            {
                It.RemoveCurrent();
            }
        }
    }

    RebuildIndex();
}

TArray<FBlueprintBreakpointResolution> FBlueprintBreakpointStore::ResolveCached(const FBlueprintSourceIdentity& Identity, const TArray<FBlueprintBreakpointRequest>& Requests) const
{
    FBlueprintSourceMap Map;
    if (!Registry.TryCopySourceMap(Identity, Map))
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("setBreakpoints while paused: no cached source map for '%s'; the request will be applied when execution resumes"), *Identity.ClassName);
        return ResolveBreakpointLines(nullptr, Requests);
    }

    return ResolveBreakpointLines(&Map.Lines, Requests);
}

TArray<FBlueprintBreakpointResolution> FBlueprintBreakpointStore::Apply(const FBlueprintSourceIdentity& Identity, const TArray<FBlueprintBreakpointRequest>& Requests, const UPackage* const PIEPackage)
{
    check(IsInGameThread());

    const TOptional<FBlueprintRenderedSource> Rendered = Registry.EnsureClassSourceFor(Identity, PIEPackage);
    const TArray<FBlueprintLineInfo>* const Lines = Rendered.IsSet() ? &Rendered->Map.Lines : nullptr;

    const TArray<FBlueprintBreakpointResolution> Resolutions = ResolveBreakpointLines(Lines, Requests);

    if (!Lines)
    {
        return Resolutions;
    }

#if PLATFORM_WINDOWS
    FBlueprintInspectorDetours::Refresh();
#endif

    // DAP replaces this source's whole set, so clear as well as set. Gather the wanted addresses first:
    // several lines can share one address, so a per-line decision would let the last line overrule the rest.
    TSet<FBreakpointKey> Wanted;
    TSet<FBreakpointTransferKey> WantedTransfers;
    for (const FBlueprintBreakpointResolution& Resolution : Resolutions)
    {
        if (Resolution.Line > 0 && Lines->IsValidIndex(Resolution.Line - 1))
        {
            const FBlueprintLineInfo& Info = (*Lines)[Resolution.Line - 1];
            if (!Info.IsBreakpointable())
            {
                continue;
            }

            // A break/continue line arms its dispatch pair instead of an address.
            if (Info.Kind == EBlueprintLineKind::LoopTransfer)
            {
                FBreakpointTransferKey Transfer;
                if (MakeBreakpointTransferKey(Info.OwnerFunction.Get(), Info.ScriptIndex, Info.TransferTarget, Transfer))
                {
                    WantedTransfers.Add(Transfer);
                }
                continue;
            }

            FBreakpointKey Key;
            if (MakeBreakpointKey(Info.OwnerFunction, Info.ScriptIndex, Key))
            {
                Wanted.Add(Key);
            }
        }
    }

    for (const FBlueprintLineInfo& Info : *Lines)
    {
        FBreakpointKey Key;
        if (!Info.IsBreakpointable() || Info.Kind == EBlueprintLineKind::LoopTransfer || !MakeBreakpointKey(Info.OwnerFunction, Info.ScriptIndex, Key))
        {
            continue;
        }

        const bool bShouldBeSet = Wanted.Contains(Key);

        if (IsArmed(Key) != bShouldBeSet)
        {
            if (ToggleNoIndex(Info.OwnerFunction, Info.ScriptIndex) == EBreakpointToggle::Rejected)
            {
                UE_LOG(LogBlueprintInspector, Warning, TEXT("breakpoint at %s.%s@%lld could not be armed: the address is not breakpointable"),
                       *Key.ClassPath.ToString(), *Key.FunctionName.ToString(), Key.Address);
            }
        }
    }

    // The transfer set follows the same replace-this-source rule. The armed pair lives outside the address
    // index, so these entries are added and dropped here rather than through Toggle.
    for (const FBlueprintLineInfo& Info : *Lines)
    {
        if (!Info.IsBreakpointable() || Info.Kind != EBlueprintLineKind::LoopTransfer)
        {
            continue;
        }

        FBreakpointTransferKey Key;
        if (!MakeBreakpointTransferKey(Info.OwnerFunction.Get(), Info.ScriptIndex, Info.TransferTarget, Key))
        {
            continue;
        }

        if (WantedTransfers.Contains(Key))
        {
            Transfers.Add(Key);
        }
        else
        {
            Transfers.Remove(Key);
        }
    }

    // Options travel with the statement line's record. A break/continue arm has no record here, so a
    // condition or log message on an arm line is ignored; the DAP response still verifies the line.
    TSet<FBreakpointKey> OptionKeys;
    for (int32 Index = 0; Index < Requests.Num(); ++Index)
    {
        if (!Resolutions.IsValidIndex(Index) || Resolutions[Index].Line <= 0)
        {
            continue;
        }

        if (!Lines->IsValidIndex(Resolutions[Index].Line - 1))
        {
            continue;
        }
        const FBlueprintLineInfo& Info = (*Lines)[Resolutions[Index].Line - 1];
        if (Info.Kind == EBlueprintLineKind::LoopTransfer)
        {
            continue;
        }

        FBreakpointKey Key;
        if (!MakeBreakpointKey(Info.OwnerFunction, Info.ScriptIndex, Key))
        {
            continue;
        }

        if (FBreakpointRecord* const Record = Records.Find(Key))
        {
            if (OptionKeys.Contains(Key))
            {
                if (Record->Condition != Requests[Index].Condition || Record->HitCondition != Requests[Index].HitCondition || Record->LogMessage != Requests[Index].LogMessage)
                {
                    UE_LOG(LogBlueprintInspector, Warning, TEXT("Two setBreakpoints requests resolved to the same statement; keeping the first request's condition, hit condition and log message."));
                }
                continue;
            }
            OptionKeys.Add(Key);

            Record->Condition = Requests[Index].Condition;
            Record->LogMessage = Requests[Index].LogMessage;
            // Restart the hit counter only when the condition that consumes it changed; a client that
            // re-sends its breakpoint set must not reset an in-progress hit condition.
            if (Record->HitCondition != Requests[Index].HitCondition)
            {
                Record->HitCount = 0;
            }
            Record->HitCondition = Requests[Index].HitCondition;
        }
    }

    // DAP replaces this source's whole set. The rendered lines name the classes this source covers, and
    // a request that clears every breakpoint still has to remove them, so scope the leftovers by those
    // classes rather than by the identity's optional display name.
    TSet<FName> SourceClassPaths;
    for (const FBlueprintLineInfo& Info : *Lines)
    {
        const FName ClassPath = GetFunctionClassPath(Info.OwnerFunction.Get());
        if (ClassPath != NAME_None)
        {
            SourceClassPaths.Add(ClassPath);
        }
    }
    for (auto It = Records.CreateIterator(); It; ++It)
    {
        if (SourceClassPaths.Contains(It.Key().ClassPath) && !Wanted.Contains(It.Key()))
        {
            It.RemoveCurrent();
        }
    }
    for (auto It = Transfers.CreateIterator(); It; ++It)
    {
        if (SourceClassPaths.Contains(It->ClassPath) && !WantedTransfers.Contains(*It))
        {
            It.RemoveCurrent();
        }
    }

    RebuildIndex();
    return Resolutions;
}

void FBlueprintBreakpointStore::Clear()
{
    check(IsInGameThread());
    Records.Reset();
    Transfers.Reset();
    Addresses.Reset();
}
