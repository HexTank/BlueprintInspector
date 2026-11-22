#include "BlueprintClassRegistry.h"

#include "BlueprintInspector.h"
#include "BlueprintScriptDisassembler.h"
#include "BlueprintScriptVisualizer_Pseudo.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "UObject/Class.h"
#include "UObject/GarbageCollection.h"
// TObjectIterator lives in UObjectIterator.h in every engine version. TFieldIterator and Cast live in
// UnrealType.h: 4.25 has no TFieldIterator in FieldIterator.h, so including that header alone only
// compiles where something else (Engine/Blueprint.h) already pulled UnrealType.h in.
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

namespace
{
// Initial capacities for one class's rendered source; they are a first guess, not a limit.
constexpr int32 RenderedTextReserveChars = 64 * 1024;
constexpr int32 RenderedLinesReserve = 4096;

// One function's rendered source: disassemble the script from InStartAddress, then let the visualizer
// lay it out. The function is pure. It owns both collaborators locally and reads no registry state.
FBlueprintRenderedSource RenderFunction(const TWeakObjectPtr<const UFunction>& InFunction, const int64 InStartAddress = 0)
{
    FBlueprintRenderedSource Rendered;

    if (!InFunction.IsValid())
    {
        return Rendered;
    }

    FBlueprintScriptDisassembler Disassembler;
    // A function with no bytecode renders as its signature alone; nothing was truncated, so it is not
    // an incomplete disassembly.
    const bool bHasScript = InFunction->Script.Num() > 0;
    if (!Disassembler.FunctionToNodes(InFunction, InStartAddress) && bHasScript)
    {
        // The graph is empty or partial; render what was captured, but name it as incomplete so a
        // missing statement is not mistaken for source that does not exist.
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Disassembly of %s from offset %lld is incomplete; the rendered source may be missing statements"), *InFunction->GetName(), InStartAddress);
    }

    FBlueprintScriptVisualizer_Pseudo ScriptVisualizer_Pseudo;
    ScriptVisualizer_Pseudo.Visualize(InFunction, Disassembler, Rendered.Text, &Rendered.Map);

    return Rendered;
}

// The editor class and a PIE duplicate share a name. Prefer the PIE class so the source map and its
// function pointers belong to the class the VM is actually running.
bool ShouldPreferClass(const UClass* const NewClass, const UClass* const CurrentClass, const UPackage* const PIEPackage)
{
    if (NewClass == nullptr)
    {
        return false;
    }

    if (CurrentClass == nullptr)
    {
        return true;
    }

    if (NewClass == CurrentClass)
    {
        return false;
    }

    if (PIEPackage != nullptr)
    {
        // GetPackage() only exists from UE5; GetOutermost() is the same package in every version.
        const bool bNewIsPIE = NewClass->GetOutermost() == PIEPackage;
        const bool bCurrentIsPIE = CurrentClass->GetOutermost() == PIEPackage;
        if (bNewIsPIE != bCurrentIsPIE)
        {
            return bNewIsPIE;
        }
    }

    // Neither (or both) is the PIE duplicate: fall back to a stable ordering rather than letting the
    // object iterator's order decide which same-named class wins.
    return NewClass->GetPathName() < CurrentClass->GetPathName();
}
} // namespace

void FBlueprintClassRegistry::SetRetargetHandler(FRetargetHandler Handler)
{
    FScopeLock Lock(&ClassHandleLock);
    OnRetargeted = MoveTemp(Handler);
}

int32 FBlueprintClassRegistry::EnsureClassHandle(const UClass* const Class)
{
    check(IsInGameThread());
    if (Class == nullptr)
    {
        return 0;
    }

    const FName ClassName = Class->GetFName();
    FRetargetHandler Handler;
    const UClass* OldClass = nullptr;
    FString OldClassName;
    bool bRetargeted = false;
    int32 Handle = 0;
    {
        // Publish the name and bind the class in one critical section: a reader must not see a handle
        // that FindClass() cannot resolve, and two threads resolving the same name must not race.
        FScopeLock Lock(&ClassHandleLock);
        if (const int32* const Existing = ClassHandleByName.Find(ClassName))
        {
            Handle = *Existing;
            const TWeakObjectPtr<const UClass>* const Bound = ClassHandles.Find(Handle);
            OldClass = Bound ? Bound->Get() : nullptr;
            if (OldClass != Class)
            {
                OldClassName = OldClass != nullptr ? OldClass->GetPathName() : FString();
                bRetargeted = true;
                InvalidateClassHandle(Handle);
                Handler = OnRetargeted;
                ClassHandles.Add(Handle, Class);
            }
        }
        else
        {
            Handle = NextClassHandle++;
            ClassHandleByName.Add(ClassName, Handle);
            ClassHandles.Add(Handle, Class);
        }
    }

    if (Handler)
    {
        Handler(Class);
    }

    if (bRetargeted)
    {
        UE_LOG(LogBlueprintInspector, Log, TEXT("Class handle %d retargeted: %s -> %s"), Handle, OldClassName.IsEmpty() ? TEXT("<none>") : *OldClassName, *Class->GetPathName());
    }
    return Handle;
}

TWeakObjectPtr<const UClass> FBlueprintClassRegistry::FindClass(const int32 Handle) const
{
    FScopeLock Lock(&ClassHandleLock);

    const TWeakObjectPtr<const UClass>* const Found = ClassHandles.Find(Handle);
    return Found ? *Found : TWeakObjectPtr<const UClass>();
}

int32 FBlueprintClassRegistry::FindHandleByName(const FName ClassName) const
{
    FScopeLock Lock(&ClassHandleLock);
    return ClassHandleByName.FindRef(ClassName);
}

TOptional<FBlueprintRenderedSource> FBlueprintClassRegistry::Find(const int32 Handle) const
{
    return SourceCache.Find(Handle);
}

bool FBlueprintClassRegistry::TryCopySource(const int32 Handle, FString& OutText) const
{
    return SourceCache.TryCopy(Handle, OutText);
}

bool FBlueprintClassRegistry::TryCopyMap(const int32 Handle, FBlueprintSourceMap& OutMap) const
{
    return SourceCache.TryCopyMap(Handle, OutMap);
}

bool FBlueprintClassRegistry::TryCopySourceMap(const FBlueprintSourceIdentity& Identity, FBlueprintSourceMap& OutMap) const
{
    FScopeLock Lock(&ClassHandleLock);
    const int32 Handle = !Identity.ClassName.IsEmpty() ? ClassHandleByName.FindRef(FName(*Identity.ClassName)) : Identity.SourceReference;
    return Handle != 0 && SourceCache.TryCopyMap(Handle, OutMap);
}

void FBlueprintClassRegistry::Store(const int32 Handle, FBlueprintRenderedSource&& Rendered)
{
    SourceCache.Store(Handle, MoveTemp(Rendered));
}

TOptional<FBlueprintRenderedSource> FBlueprintClassRegistry::EnsureClassSource(const int32 ID)
{
    check(IsInGameThread());
    TOptional<FBlueprintRenderedSource> Cached = SourceCache.Find(ID);
    if (Cached.IsSet())
    {
        return MoveTemp(Cached);
    }

    // The class is resolved under the lock and rendered after releasing it: no engine call runs while
    // the handle lock is held. The weak pointer is pinned for the render so a GC cannot collect it
    // between the resolve and the function walk.
    FGCScopeGuard GcGuard;
    const TWeakObjectPtr<const UClass> FoundClass = FindClass(ID);
    const UClass* const Class = FoundClass.Get();
    if (Class == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("No class is bound to source handle %d"), ID);
        return TOptional<FBlueprintRenderedSource>();
    }

    if (!IsValid(Class))
    {
        return TOptional<FBlueprintRenderedSource>();
    }

    TArray<UObject*> Functions;
    GatherAllFunctionsFromClass(Class, Functions);

    TArray<const UFunction*> UbergraphFunctions;
    for (const UObject* const FunctionObject : Functions)
    {
        const UFunction* const Function = Cast<UFunction>(FunctionObject);
        if (Function && Function->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            UbergraphFunctions.Add(Function);
        }
    }

    FBlueprintRenderedSource Rendered;
    Rendered.Text.Reserve(RenderedTextReserveChars);
    Rendered.Map.Lines.Reserve(RenderedLinesReserve);

    const auto AppendFunctionRendered = [&Rendered](const FBlueprintRenderedSource& FunctionRendered) {
        const bool bFunctionTextEndsWithNewline = !FunctionRendered.Text.IsEmpty() && FunctionRendered.Text[FunctionRendered.Text.Len() - 1] == TEXT('\n');

        Rendered.Text += FunctionRendered.Text;
        Rendered.Text += TEXT("\n");
        Rendered.Map.Lines.Append(FunctionRendered.Map.Lines);

        if (bFunctionTextEndsWithNewline || FunctionRendered.Text.IsEmpty())
        {
            Rendered.Map.Lines.AddDefaulted();
        }
    };

    // Event stubs call ExecuteUbergraph_<Class>(N); the integer is the entry offset inside the
    // ubergraph's jump-table-shaped script. One pass over the text finds every call site, and an offset
    // is only recorded when it addresses the ubergraph's script.
    // The scan below sees the call as `ExecuteUbergraph_<Class>(N)`, so the lookup key is the part
    // after the prefix; the entry set itself stays keyed by the full function name.
    const FString UbergraphPrefix = TEXT("ExecuteUbergraph_");
    TMap<FName, const UFunction*> UbergraphByName;
    for (const UFunction* const Ubergraph : UbergraphFunctions)
    {
        const FString FunctionName = Ubergraph->GetName();
        const FString EntryKey = FunctionName.StartsWith(UbergraphPrefix) ? FunctionName.Mid(UbergraphPrefix.Len()) : FunctionName;
        UbergraphByName.Add(FName(*EntryKey), Ubergraph);
    }

    TMap<FName, TSet<int64>> EntryPoints;
    for (const UObject* const FunctionObject : Functions)
    {
        const UFunction* const Function = Cast<UFunction>(FunctionObject);
        if (Function == nullptr || Function->HasAllFunctionFlags(FUNC_UbergraphFunction))
        {
            continue;
        }

        const FBlueprintRenderedSource FunctionRendered = RenderFunction(Function);
        AppendFunctionRendered(FunctionRendered);

        int32 Search = 0;
        int32 Position = INDEX_NONE;
        bool bInString = false;
        bool bInLineComment = false;
        bool bInBlockComment = false;
        TCHAR Quote = 0;
        while ((Position = FunctionRendered.Text.Find(UbergraphPrefix, ESearchCase::CaseSensitive, ESearchDir::FromStart, Search)) != INDEX_NONE)
        {
            for (int32 Index = Search; Index < Position; ++Index)
            {
                const TCHAR Character = FunctionRendered.Text[Index];
                if (bInLineComment)
                {
                    if (Character == TEXT('\n'))
                    {
                        bInLineComment = false;
                    }
                    continue;
                }
                if (bInBlockComment)
                {
                    if (Character == TEXT('*') && Index + 1 < Position && FunctionRendered.Text[Index + 1] == TEXT('/'))
                    {
                        bInBlockComment = false;
                        ++Index;
                    }
                    continue;
                }
                if (bInString)
                {
                    if (Character == TEXT('\\') && Index + 1 < Position)
                    {
                        ++Index;
                    }
                    else if (Character == Quote)
                    {
                        bInString = false;
                    }
                    continue;
                }
                if (Character == TEXT('/') && Index + 1 < Position)
                {
                    const TCHAR Next = FunctionRendered.Text[Index + 1];
                    if (Next == TEXT('/'))
                    {
                        bInLineComment = true;
                        ++Index;
                        continue;
                    }
                    if (Next == TEXT('*'))
                    {
                        bInBlockComment = true;
                        ++Index;
                        continue;
                    }
                }
                if (Character == TEXT('"') || Character == TEXT('\''))
                {
                    bInString = true;
                    Quote = Character;
                }
            }

            if (bInString || bInLineComment || bInBlockComment || (Position > 0 && !FChar::IsWhitespace(FunctionRendered.Text[Position - 1])))
            {
                Search = Position + UbergraphPrefix.Len();
                continue;
            }

            int32 NameEnd = Position + UbergraphPrefix.Len();
            while (NameEnd < FunctionRendered.Text.Len() && (FChar::IsAlnum(FunctionRendered.Text[NameEnd]) || FunctionRendered.Text[NameEnd] == TEXT('_')))
            {
                ++NameEnd;
            }

            const UFunction* const Ubergraph = UbergraphByName.FindRef(FName(*FunctionRendered.Text.Mid(Position + UbergraphPrefix.Len(), NameEnd - (Position + UbergraphPrefix.Len()))));
            if (Ubergraph == nullptr || NameEnd >= FunctionRendered.Text.Len() || FunctionRendered.Text[NameEnd] != TEXT('('))
            {
                Search = NameEnd;
                continue;
            }

            int32 NumberEnd = NameEnd + 1;
            while (NumberEnd < FunctionRendered.Text.Len() && FChar::IsDigit(FunctionRendered.Text[NumberEnd]))
            {
                ++NumberEnd;
            }

            if (NumberEnd > NameEnd + 1 && NumberEnd < FunctionRendered.Text.Len() && FunctionRendered.Text[NumberEnd] == TEXT(')'))
            {
                const int64 Offset = FCString::Atoi64(*FunctionRendered.Text.Mid(NameEnd + 1, NumberEnd - (NameEnd + 1)));
                if (Offset >= 0 && Offset < Ubergraph->Script.Num())
                {
                    EntryPoints.FindOrAdd(Ubergraph->GetFName()).Add(Offset);
                }
            }

            Search = NumberEnd;
        }
    }

    for (const UFunction* const Ubergraph : UbergraphFunctions)
    {
        const TSet<int64>* const EntriesSet = EntryPoints.Find(Ubergraph->GetFName());
        TArray<int64> Entries = EntriesSet ? EntriesSet->Array() : TArray<int64>();
        Entries.Sort();

        if (Entries.Num() == 0)
        {
            AppendFunctionRendered(RenderFunction(Ubergraph));
            continue;
        }

        // Each entry renders the ubergraph from its own offset. The offset goes into the rendered name
        // (`ExecuteUbergraph_X_1403`) so the sections are distinct instead of several identical
        // signatures; the event stubs that call in still show the numeric form the bytecode carries.
        for (const int64 Entry : Entries)
        {
            FBlueprintRenderedSource EntryRendered = RenderFunction(Ubergraph, Entry);
            EntryRendered.Text = UbergraphEntrySignature(EntryRendered.Text, Entry);
            AppendFunctionRendered(EntryRendered);
        }
    }

    const int32 LineCount = CountRenderedLines(Rendered.Text);
    if (Rendered.Map.Lines.Num() != LineCount)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Rendered source map for handle %d has %d line(s) for %d rendered line(s); dropping the mapping"), ID, Rendered.Map.Lines.Num(), LineCount);
        Rendered.Map.Lines.Empty(LineCount);
        Rendered.Map.Lines.AddDefaulted(FMath::Max(0, LineCount));
    }

    Rendered.Map.BuildReverseIndex();

    {
        FScopeLock Lock(&ClassHandleLock);
        const TWeakObjectPtr<const UClass>* const Bound = ClassHandles.Find(ID);
        if (Bound == nullptr || Bound->Get() != Class)
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Source for handle %d was rendered for a class the handle no longer names; discarding"), ID);
            return TOptional<FBlueprintRenderedSource>();
        }
        SourceCache.Store(ID, MoveTemp(Rendered));
    }
    return SourceCache.Find(ID);
}

FString FBlueprintClassRegistry::AcquireClassFunctions(const int32 ID)
{
    TOptional<FBlueprintRenderedSource> Rendered = EnsureClassSource(ID);
    return Rendered.IsSet() ? MoveTemp(Rendered->Text) : FString();
}

void FBlueprintClassRegistry::GatherAllFunctionsFromClass(const UClass* const InClass, TArray<UObject*>& OutFunctions, const EFunctionFlags InFunctionHasAllFlags, const EFunctionFlags InFunctionDoesNotHaveAllFlags)
{
    OutFunctions.Empty();
    if (InClass)
    {
        for (TFieldIterator<UFunction> FunctionIterator(InClass, EFieldIteratorFlags::ExcludeSuper); FunctionIterator; ++FunctionIterator)
        {
            if (InFunctionHasAllFlags != FUNC_None && !(*FunctionIterator)->HasAllFunctionFlags(InFunctionHasAllFlags))
            {
                continue;
            }

            if (InFunctionDoesNotHaveAllFlags != FUNC_None && (*FunctionIterator)->HasAllFunctionFlags(InFunctionDoesNotHaveAllFlags))
            {
                continue;
            }

            OutFunctions.Add(*FunctionIterator);
        }
    }
}

TArray<FFunctionProperties> FBlueprintClassRegistry::AcquireClassList(const UPackage* const PIEPackage)
{
    check(IsInGameThread());
    // A PIE session creates new class objects and the old ones are collected. Drop their handles and
    // name entries here, where the list is rebuilt anyway, so repeated sessions do not grow the maps.
    {
        FScopeLock Lock(&ClassHandleLock);
        for (auto It = ClassHandles.CreateIterator(); It; ++It)
        {
            if (!It.Value().IsValid())
            {
                InvalidateClassHandle(It.Key());
                It.RemoveCurrent();
            }
        }
        for (auto It = ClassHandleByName.CreateIterator(); It; ++It)
        {
            if (!ClassHandles.Contains(It.Value()))
            {
                It.RemoveCurrent();
            }
        }
    }

    TArray<FFunctionProperties> Functions;
    const TArray<UObject*> Classes = GatherBlueprintClasses();

    // The registry key is the one class per name the VM would run: the PIE duplicate when a PIE world
    // exists, otherwise the editor class.
    TMap<FName, const UClass*> PreferredClasses;
    TArray<FName> ClassOrder;
    for (const UObject* const ClassObject : Classes)
    {
        const UClass* const Class = Cast<UClass>(ClassObject);
        if (!Class)
        {
            continue;
        }

        const FName Name = Class->GetFName();
        if (const UClass** const Existing = PreferredClasses.Find(Name))
        {
            if (ShouldPreferClass(Class, *Existing, PIEPackage))
            {
                *Existing = Class;
            }
        }
        else
        {
            PreferredClasses.Add(Name, Class);
            ClassOrder.Add(Name);
        }
    }

    Functions.Reserve(ClassOrder.Num());
    for (const FName& Name : ClassOrder)
    {
        const UClass* const Class = PreferredClasses.FindRef(Name);
        if (!Class)
        {
            continue;
        }

        // Issuing the handle also binds it, which retargets the class a stale handle used to name.
        const int32 Handle = EnsureClassHandle(Class);
        Functions.Add({Name, Handle});
    }

    return Functions;
}

TOptional<FBlueprintRenderedSource> FBlueprintClassRegistry::EnsureClassSourceFor(const FBlueprintSourceIdentity& Identity, const UPackage* const PIEPackage)
{
    check(IsInGameThread());
    if (!Identity.ClassName.IsEmpty())
    {
        const FName ClassName(*Identity.ClassName);

        int32 NamedHandle = FindHandleByName(ClassName);
        if (NamedHandle != 0)
        {
            if (Identity.SourceReference != 0 && Identity.SourceReference != NamedHandle)
            {
                UE_LOG(LogBlueprintInspector, Warning, TEXT("sourceReference %d does not name '%s' (that class is handle %d) - resolving by the class name"), Identity.SourceReference, *Identity.ClassName, NamedHandle);
            }

            TOptional<FBlueprintRenderedSource> NamedSource = EnsureClassSource(NamedHandle);
            if (NamedSource.IsSet())
            {
                return NamedSource;
            }
        }

        // List on demand so a client can replay persisted breakpoints before asking for anything.
        AcquireClassList(PIEPackage);

        NamedHandle = FindHandleByName(ClassName);
        if (NamedHandle != 0)
        {
            return EnsureClassSource(NamedHandle);
        }

        UE_LOG(LogBlueprintInspector, Warning, TEXT("No class named '%s' to render"), *Identity.ClassName);
        return TOptional<FBlueprintRenderedSource>();
    }

    if (Identity.SourceReference == 0)
    {
        return TOptional<FBlueprintRenderedSource>();
    }

    TOptional<FBlueprintRenderedSource> Rendered = EnsureClassSource(Identity.SourceReference);
    if (!Rendered.IsSet())
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("No source for sourceReference %d"), Identity.SourceReference);
    }

    return Rendered;
}

TArray<UObject*> FBlueprintClassRegistry::GatherBlueprintClasses() const
{
    TArray<UObject*> BlueprintClasses;
    GatherAllClasses(BlueprintClasses, CLASS_CompiledFromBlueprint, FUNC_None, FUNC_None);
    return BlueprintClasses;
}

void FBlueprintClassRegistry::GatherAllClasses(TArray<UObject*>& OutClasses, const EClassFlags InClassHasAllFlags, const EFunctionFlags InFunctionHasAllFlags, const EFunctionFlags InFunctionDoesNotHaveAllFlags)
{
    OutClasses.Empty();
    for (TObjectIterator<UClass> ObjectIterator; ObjectIterator; ++ObjectIterator)
    {
        if (const UClass* const Class = *ObjectIterator)
        {
            if (InClassHasAllFlags != CLASS_None && !Class->HasAllClassFlags(InClassHasAllFlags))
            {
                continue;
            }

            TFieldIterator<UFunction> FunctionIterator(Class, EFieldIteratorFlags::ExcludeSuper);
            if (!FunctionIterator)
            {
                continue;
            }

            for (; FunctionIterator; ++FunctionIterator)
            {
                if (InFunctionHasAllFlags != FUNC_None && !(*FunctionIterator)->HasAllFunctionFlags(InFunctionHasAllFlags))
                {
                    continue;
                }

                if (InFunctionDoesNotHaveAllFlags != FUNC_None && (*FunctionIterator)->HasAllFunctionFlags(InFunctionDoesNotHaveAllFlags))
                {
                    continue;
                }

                OutClasses.Add(*ObjectIterator);
                break;
            }
        }
    }
    TArray<TPair<FString, UObject*>> Paths;
    Paths.Reserve(OutClasses.Num());
    for (UObject* const Class : OutClasses)
    {
        Paths.Emplace(Class->GetPathName(), Class);
    }
    Paths.Sort([](const TPair<FString, UObject*>& A, const TPair<FString, UObject*>& B) { return A.Key < B.Key; });
    OutClasses.Reset(Paths.Num());
    for (const TPair<FString, UObject*>& Path : Paths)
    {
        OutClasses.Add(Path.Value);
    }
}

void FBlueprintClassRegistry::CopyCachedClassList(TArray<FFunctionProperties>& OutFunctions) const
{
    FScopeLock Lock(&ClassHandleLock);

    OutFunctions.Reset();
    OutFunctions.Reserve(ClassHandles.Num());
    for (const TPair<int32, TWeakObjectPtr<const UClass>>& Pair : ClassHandles)
    {
        if (const UClass* const Class = Pair.Value.Get())
        {
            OutFunctions.Add({Class->GetFName(), Pair.Key});
        }
    }
}

void FBlueprintClassRegistry::CopyCachedClassIdentities(TMap<FName, FCachedClassIdentity>& OutByIdentity) const
{
    FScopeLock Lock(&ClassHandleLock);

    OutByIdentity.Reset();
    OutByIdentity.Reserve(ClassHandles.Num());
    for (const TPair<int32, TWeakObjectPtr<const UClass>>& Pair : ClassHandles)
    {
        if (const UClass* const Class = Pair.Value.Get())
        {
            OutByIdentity.Add(GetClassPath(Class), FCachedClassIdentity{Class->GetFName().ToString(), Pair.Key});
        }
    }
}

void FBlueprintClassRegistry::InvalidateClassHandle(const int32 Handle)
{
    SourceCache.Invalidate(Handle);
}
