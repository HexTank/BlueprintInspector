#include "BlueprintPauseSession.h"

#include "BlueprintExpression.h"
#include "BlueprintInspectorPort.h"
#include "BlueprintInspectorVariableEdit.h"
#include "BlueprintSourceMap.h"
#include "HAL/PlatformProcess.h"
#include "Misc/ScopeLock.h"
#include "UObject/Class.h"
// TFieldIterator, CastField and the FScript*Helper classes live here in 4.25 as well as 5.x, so this is
// the portable include (see the same note in BlueprintClassRegistry.cpp).
#include "UObject/UnrealType.h"
// FFrame is forward-declared in Class.h and defined here, on both 4.25 and 5.x.
#include "UObject/Stack.h"

namespace
{
constexpr double VariableWriteTimeoutSeconds = 5.0;
constexpr int32 MinContainerDisplayNameLength = 3; // "[N]"

int64 GetCallerFrameAddress(const FFrame& InFrame)
{
    if (InFrame.Node == nullptr || InFrame.Code == nullptr || InFrame.Node->Script.Num() == 0)
    {
        return INDEX_NONE;
    }

    const UPTRINT ScriptStart = reinterpret_cast<UPTRINT>(InFrame.Node->Script.GetData());
    const UPTRINT ScriptEnd = ScriptStart + static_cast<UPTRINT>(InFrame.Node->Script.Num());
    const UPTRINT Code = reinterpret_cast<UPTRINT>(InFrame.Code);
    return Code >= ScriptStart && Code <= ScriptEnd ? static_cast<int64>(Code - ScriptStart) : INDEX_NONE;
}

// A container element's display name is "[<index>]"; anything else names a property.
bool ParseContainerIndex(const FString& Name, int32& OutIndex)
{
    if (Name.Len() < MinContainerDisplayNameLength || Name[0] != TEXT('[') || Name[Name.Len() - 1] != TEXT(']'))
    {
        return false;
    }

    const FString IndexText = Name.Mid(1, Name.Len() - 2);
    for (int32 Index = 0; Index < IndexText.Len(); ++Index)
    {
        if (!FChar::IsDigit(IndexText[Index]))
        {
            return false;
        }
    }

    const int64 ParsedIndex = FCString::Atoi64(*IndexText);
    if (ParsedIndex < 0 || ParsedIndex > TNumericLimits<int32>::Max())
    {
        return false;
    }

    OutIndex = static_cast<int32>(ParsedIndex);
    return true;
}
} // namespace

FBlueprintPauseSession::FBlueprintPauseSession(FBlueprintClassRegistry& InRegistry) : Registry(InRegistry)
{
}

void FBlueprintPauseSession::CreatePauseEvent()
{
    FScopeLock Guard(&RequestLock);

    Event = TSharedPtr<FEvent, ESPMode::ThreadSafe>(FPlatformProcess::GetSynchEventFromPool(true), [](FEvent* InEvent) { FPlatformProcess::ReturnSynchEventToPool(InEvent); });
}

TSharedPtr<FEvent, ESPMode::ThreadSafe> FBlueprintPauseSession::GetEvent() const
{
    FScopeLock Guard(&RequestLock);
    return Event;
}

TSharedPtr<FEvent, ESPMode::ThreadSafe> FBlueprintPauseSession::TakeEvent()
{
    FScopeLock Guard(&RequestLock);

    TSharedPtr<FEvent, ESPMode::ThreadSafe> Taken = MoveTemp(Event);
    return Taken;
}

bool FBlueprintPauseSession::IsPaused() const
{
    return PauseState.Observe();
}

bool FBlueprintPauseSession::IsPausedTakingRequestLock() const
{
    return PauseState.ObserveLocked();
}

void FBlueprintPauseSession::PublishPaused()
{
    PauseState.PublishPaused();
}

void FBlueprintPauseSession::EndPause()
{
    // One transition: the flag clears and the queue is taken together, so a write that arrives from here
    // on is rejected instead of waiting for an apply that will never run.
    const TArray<TSharedPtr<FBlueprintVariableWriteRequest>> Pending = PauseState.EndPause();

    for (const TSharedPtr<FBlueprintVariableWriteRequest>& Request : Pending)
    {
        Request->Cancel();
    }

    ClearFrame();
}

bool FBlueprintPauseSession::QueueBreakpointsIfPaused(FBlueprintPendingBreakpoints Pending)
{
    FScopeLock Guard(&RequestLock);
    if (!PauseState.Observe())
    {
        return false;
    }

    // The resume reads the flag and drains this queue under the same lock, and applies it before the VM
    // runs another opcode.
    PendingBreakpoints.Add(MoveTemp(Pending));
    return true;
}

TArray<FBlueprintPendingBreakpoints> FBlueprintPauseSession::TakePendingBreakpoints()
{
    FScopeLock Guard(&RequestLock);

    TArray<FBlueprintPendingBreakpoints> Taken = MoveTemp(PendingBreakpoints);
    PendingBreakpoints.Reset();
    return Taken;
}

void FBlueprintPauseSession::DropPendingBreakpoints()
{
    FScopeLock Guard(&RequestLock);
    PendingBreakpoints.Reset();
}

FBlueprintSetVariableResult FBlueprintPauseSession::SetVariable(const int32 VariablesReference, const FString& Name, const FString& Value)
{
    if (IsInGameThread())
    {
        return SetVariableOnGameThread(VariablesReference, Name, Value);
    }

    TSharedPtr<FBlueprintVariableWriteRequest> Request = MakeShared<FBlueprintVariableWriteRequest>(VariablesReference, Name, Value);

    // The pause flag and the queue are one state: the handshake either queues this request for a parked
    // loop that will drain it, or answers "not paused". Checking the flag outside the enqueue could see a
    // pause that a resume ends before the request is queued, leaving it for a loop that has stopped.
    const FBlueprintPauseState::EWriteEnqueueResult Enqueued = PauseState.EnqueueIfPaused(Request);
    if (Enqueued != FBlueprintPauseState::EWriteEnqueueResult::Queued)
    {
        FBlueprintSetVariableResult Result;
        Result.Error = Enqueued == FBlueprintPauseState::EWriteEnqueueResult::QueueFull
                           ? TEXT("The debugger has too many pending variable writes; try again after it resumes.")
                           : TEXT("The debugger is not paused.");
        return Result;
    }

    return Request->Wait(FTimespan::FromSeconds(VariableWriteTimeoutSeconds));
}

void FBlueprintPauseSession::ProcessPendingWrites()
{
    // Taking the queue leaves the pause in place: the loop that is parked keeps draining it.
    const TArray<TSharedPtr<FBlueprintVariableWriteRequest>> Pending = PauseState.TakeQueuedWrites();

    for (const TSharedPtr<FBlueprintVariableWriteRequest>& Request : Pending)
    {
        Request->Apply([this, Request]() {
            return SetVariableOnGameThread(Request->GetVariablesReference(), Request->GetName(), Request->GetValue());
        });
    }
}

void FBlueprintPauseSession::PublishFrame(const FFrame& InFrame, const int64 Address, const int32 LineOverride)
{
    struct FBuiltFrame
    {
        FBlueprintPausedFrame Frame;
        FBlueprintVariableGroup Parameters;
        FBlueprintVariableGroup Locals;
        FBlueprintVariableGroup ClassGroup;
        bool bHasClassGroup = false;
    };

    uint64 StartGeneration = 0;
    {
        FScopeLock Guard(&Lock);
        StartGeneration = Generation;
    }

    // The VM chains a new FFrame for every script call, so PreviousFrame walks the callers. Native frames
    // reuse the caller's frame and have no script of their own, so they are skipped.
    TArray<FBuiltFrame> Built;
    for (const FFrame* Frame = &InFrame; Frame != nullptr; Frame = Frame->PreviousFrame)
    {
        const UFunction* const Function = Frame->Node;
        if (Function == nullptr || Function->Script.Num() == 0)
        {
            continue;
        }

        FBuiltFrame Entry;
        FBlueprintPausedFrame& Paused = Entry.Frame;
        Paused.Function = Function;
        Paused.Class = Function->GetOwnerClass();
        Paused.ClassHandle = Registry.EnsureClassHandle(Paused.Class);

        if (Frame == &InFrame)
        {
            Paused.Address = Address;
        }
        else
        {
            // A caller is suspended inside the call, so Code has already passed the call opcode and its
            // operands; the exact address lookup cannot find the call site.
            Paused.Address = GetCallerFrameAddress(*Frame);
        }

        // Render each class before the game thread parks. The client asks for a source as soon as the
        // stopped event arrives, and that request cannot wait on a parked game thread.
        if (Paused.ClassHandle != 0)
        {
            Registry.EnsureClassSource(Paused.ClassHandle);
            Paused.Line = Frame == &InFrame ? (LineOverride > 0 ? LineOverride : FindLine(Paused.ClassHandle, Function, Paused.Address)) : FindLineAtOrBefore(Paused.ClassHandle, Function, Paused.Address);
        }

        Entry.Parameters.Kind = EBlueprintVariableGroupKind::Parameters;
        Entry.Parameters.Function = Function;
        Entry.Parameters.Locals = Frame->Locals;
        Entry.Parameters.Object = Frame->Object;

        Entry.Locals = Entry.Parameters;
        Entry.Locals.Kind = EBlueprintVariableGroupKind::Locals;

        if (Frame->Object != nullptr)
        {
            Entry.ClassGroup.Kind = EBlueprintVariableGroupKind::Class;
            Entry.ClassGroup.Object = Frame->Object;
            Entry.bHasClassGroup = true;
        }

        Built.Add(MoveTemp(Entry));
    }

#if WITH_DEV_AUTOMATION_TESTS
    if (TestBeforePublish)
    {
        TestBeforePublish();
    }
#endif

    FScopeLock Guard(&Lock);
    const bool bClearedDuringBuild = Generation != StartGeneration;
    if (bClearedDuringBuild)
    {
        return;
    }

    Frames.Reset();
    Groups.Reset();
    Frames.Reserve(Built.Num());
    // References are issued once and never reused. Resetting the counter would let a reference retained
    // from the previous pause resolve to a different group of the new one.
    ++Generation;

    for (FBuiltFrame& Entry : Built)
    {
        FBlueprintPausedFrame& Paused = Entry.Frame;
        Paused.ParametersReference = AllocateGroupLocked(Entry.Parameters);
        Paused.LocalsReference = AllocateGroupLocked(Entry.Locals);
        if (Entry.bHasClassGroup)
        {
            Paused.ClassReference = AllocateGroupLocked(Entry.ClassGroup);
        }
        Frames.Add(Paused);
    }
}

#if WITH_DEV_AUTOMATION_TESTS
int64 FBlueprintPauseSession::GetCallerFrameAddressForTest(const FFrame& InFrame)
{
    return GetCallerFrameAddress(InFrame);
}
#endif

void FBlueprintPauseSession::ClearFrame()
{
    FScopeLock Guard(&Lock);

    Frames.Reset();
    Groups.Reset();
    ++Generation;
}

TArray<FBlueprintStackFrame> FBlueprintPauseSession::AcquireStackFrames() const
{
    FScopeLock Guard(&Lock);

    TArray<FBlueprintStackFrame> Out;
    Out.Reserve(Frames.Num());

    for (const FBlueprintPausedFrame& Paused : Frames)
    {
        FBlueprintStackFrame Frame;
        Frame.Name = Paused.Function->GetName();
        Frame.ClassName = Paused.Class ? Paused.Class->GetName() : FString();
        Frame.SourceReference = Paused.ClassHandle;
        Frame.Line = Paused.Line;
        Out.Add(MoveTemp(Frame));
    }

    return Out;
}

TArray<FBlueprintScope> FBlueprintPauseSession::AcquireScopes(const int32 FrameId) const
{
    FScopeLock Guard(&Lock);

    TArray<FBlueprintScope> Scopes;
    if (!Frames.IsValidIndex(FrameId - 1))
    {
        return Scopes;
    }

    const FBlueprintPausedFrame& Paused = Frames[FrameId - 1];

    FBlueprintScope Parameters;
    Parameters.Name = TEXT("Parameters");
    Parameters.VariablesReference = Paused.ParametersReference;
    Scopes.Add(MoveTemp(Parameters));

    FBlueprintScope Locals;
    Locals.Name = TEXT("Locals");
    Locals.VariablesReference = Paused.LocalsReference;
    Scopes.Add(MoveTemp(Locals));

    if (Paused.ClassReference != 0)
    {
        FBlueprintScope This;
        This.Name = TEXT("This");
        This.VariablesReference = Paused.ClassReference;
        Scopes.Add(MoveTemp(This));
    }

    return Scopes;
}

TArray<FBlueprintVariable> FBlueprintPauseSession::AcquireVariables(const int32 VariablesReference) const
{
    FScopeLock Guard(&Lock);

    TArray<FBlueprintVariable> Variables;
    const FBlueprintVariableGroup* const FoundGroup = Groups.Find(VariablesReference);
    if (!FoundGroup)
    {
        return Variables;
    }

    const FBlueprintVariableGroup Group = *FoundGroup;

    if (!Group.IsLive(Generation))
    {
        return Variables;
    }

    if (Group.Kind == EBlueprintVariableGroupKind::Class)
    {
        const UObject* const Object = Group.Object;
        if (Object == nullptr)
        {
            return Variables;
        }

        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
            const FProperty* const Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Deprecated | CPF_Parm))
            {
                continue;
            }
            Variables.Add(MakeVariableLocked(Property, reinterpret_cast<const uint8*>(Object), Object, FString(), Group.Access));
        }
    }
    else if (Group.Kind == EBlueprintVariableGroupKind::Parameters || Group.Kind == EBlueprintVariableGroupKind::Locals)
    {
        if (Group.Function == nullptr || Group.Locals == nullptr)
        {
            return Variables;
        }

        for (TFieldIterator<FProperty> It(Group.Function); It; ++It)
        {
            const FProperty* const Property = *It;
            if (Property->HasAnyPropertyFlags(CPF_Deprecated))
            {
                continue;
            }
            if (Property->HasAnyPropertyFlags(CPF_Parm) != (Group.Kind == EBlueprintVariableGroupKind::Parameters))
            {
                continue;
            }
            Variables.Add(MakeVariableLocked(Property, Group.Locals, Group.Object, FString(), Group.Access));
        }
    }
    else if (Group.Kind == EBlueprintVariableGroupKind::Struct && Group.Struct && Group.Address)
    {
        for (TFieldIterator<FProperty> It(Group.Struct); It; ++It)
        {
            Variables.Add(MakeVariableLocked(*It, Group.Address, Group.Object, FString(), Group.Access));
        }
    }
    else if (Group.Kind == EBlueprintVariableGroupKind::Array && Group.Property && Group.Address)
    {
        const FArrayProperty* const ArrayProperty = CastField<FArrayProperty>(Group.Property);
        if (ArrayProperty && ArrayProperty->Inner)
        {
            FScriptArrayHelper Helper(ArrayProperty, Group.Address);
            for (int32 Index = 0; Index < Helper.Num(); ++Index)
            {
                Variables.Add(MakeVariableLocked(ArrayProperty->Inner, Helper.GetRawPtr(Index), Group.Object, FString::Printf(TEXT("[%d]"), Index), Group.Access));
            }
        }
    }
    else if (Group.Kind == EBlueprintVariableGroupKind::Set && Group.Property && Group.Address)
    {
        const FSetProperty* const SetProperty = CastField<FSetProperty>(Group.Property);
        if (SetProperty && SetProperty->ElementProp)
        {
            FScriptSetHelper Helper(SetProperty, Group.Address);
            int32 DisplayIndex = 0;
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index))
                {
                    continue;
                }
                // Elements are hashed by the set, so every view of one is read-only.
                Variables.Add(MakeVariableLocked(SetProperty->ElementProp, Helper.GetElementPtr(Index), Group.Object, FString::Printf(TEXT("[%d]"), DisplayIndex++), EBlueprintVariableGroupAccess::ReadOnly));
            }
        }
    }
    else if (Group.Kind == EBlueprintVariableGroupKind::Map && Group.Property && Group.Address)
    {
        const FMapProperty* const MapProperty = CastField<FMapProperty>(Group.Property);
        if (MapProperty && MapProperty->ValueProp)
        {
            FScriptMapHelper Helper(MapProperty, Group.Address);
            int32 DisplayIndex = 0;
            for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
            {
                if (!Helper.IsValidIndex(Index))
                {
                    continue;
                }
                Variables.Add(MakeVariableLocked(MapProperty->ValueProp, Helper.GetValuePtr(Index), Group.Object, FString::Printf(TEXT("[%d]"), DisplayIndex++), Group.Access));
            }
        }
    }

    return Variables;
}

FBlueprintEvaluateResult FBlueprintPauseSession::EvaluateExpression(const int32 FrameId, const FString& Expression) const
{
    FBlueprintEvaluateResult Result;
    FScopeLock Guard(&Lock);

    if (Frames.Num() == 0)
    {
        Result.Error = TEXT("execution is not paused");
        return Result;
    }

    if (!Frames.IsValidIndex(FrameId - 1))
    {
        Result.Error = FString::Printf(TEXT("frame %d is not part of the paused stack"), FrameId);
        return Result;
    }

    const FBlueprintPausedFrame& Frame = Frames[FrameId - 1];

    FBlueprintExpressionContext Context;
    Context.Function = Frame.Function;
    const FBlueprintVariableGroup* const Locals = Groups.Find(Frame.LocalsReference);
    const FBlueprintVariableGroup* const ClassGroup = Groups.Find(Frame.ClassReference);
    if ((Locals != nullptr && !Locals->IsLive(Generation)) || (ClassGroup != nullptr && !ClassGroup->IsLive(Generation)))
    {
        Result.Error = TEXT("The paused frame is no longer valid; refresh the paused stack.");
        return Result;
    }
    if (Locals != nullptr)
    {
        Context.Locals = Locals->Locals;
        Context.Object = Locals->Object;
    }
    if (ClassGroup != nullptr)
    {
        if (ClassGroup->Object != nullptr)
        {
            Context.Object = ClassGroup->Object;
        }
    }

    FBlueprintExpressionValue Value;
    FString Error;
    if (!BlueprintExpression::EvaluateValue(Context, Expression, Value, Error))
    {
        Result.Error = Error;
        return Result;
    }

    Result.bSuccess = true;
    Result.Result = Value.Display;
    Result.Type = Value.Type;
    return Result;
}

FBlueprintSetVariableResult FBlueprintPauseSession::SetVariableOnGameThread(const int32 VariablesReference, const FString& Name, const FString& Value)
{
    FScopeLock Guard(&Lock);

    FBlueprintSetVariableResult Result;
    const FBlueprintVariableGroup* const FoundGroup = Groups.Find(VariablesReference);
    if (FoundGroup == nullptr)
    {
        Result.Error = TEXT("The variable reference is no longer valid; refresh the paused frame.");
        return Result;
    }

    const FBlueprintVariableGroup Group = *FoundGroup;

    if (!Group.IsLive(Generation))
    {
        Result.Error = TEXT("The variable reference is no longer valid; refresh the paused frame.");
        return Result;
    }

    const FProperty* Property = nullptr;
    uint8* Container = nullptr;

    // A set hashes its elements, so writing one in place (directly, or through a struct/container group
    // that points into it) would leave the table inconsistent with its contents. Editing an element
    // needs remove-then-reinsert, which this path does not implement.
    if (Group.Kind == EBlueprintVariableGroupKind::Set || Group.Access == EBlueprintVariableGroupAccess::ReadOnly)
    {
        Result.Error = TEXT("Set elements cannot be edited in place; removing and re-adding the element would be required.");
        return Result;
    }

    if (Group.Kind == EBlueprintVariableGroupKind::Class)
    {
        if (Group.Object == nullptr)
        {
            Result.Error = TEXT("This scope has no object.");
            return Result;
        }
        for (TFieldIterator<FProperty> It(Group.Object->GetClass()); It; ++It)
        {
            const FProperty* const Candidate = *It;
            if (Candidate->GetName() == Name && !Candidate->HasAnyPropertyFlags(CPF_Deprecated | CPF_Parm))
            {
                Property = Candidate;
                break;
            }
        }
        Container = const_cast<uint8*>(reinterpret_cast<const uint8*>(Group.Object));
    }
    else if (Group.Kind == EBlueprintVariableGroupKind::Parameters || Group.Kind == EBlueprintVariableGroupKind::Locals)
    {
        if (Group.Function == nullptr || Group.Locals == nullptr)
        {
            Result.Error = TEXT("The frame's locals are no longer available.");
            return Result;
        }
        const bool bParameters = Group.Kind == EBlueprintVariableGroupKind::Parameters;
        for (TFieldIterator<FProperty> It(Group.Function); It; ++It)
        {
            const FProperty* const Candidate = *It;
            if (Candidate->GetName() != Name || Candidate->HasAnyPropertyFlags(CPF_Deprecated) || Candidate->HasAnyPropertyFlags(CPF_Parm) != bParameters)
            {
                continue;
            }
            Property = Candidate;
            break;
        }
        Container = const_cast<uint8*>(Group.Locals);
    }
    else if (Group.Kind == EBlueprintVariableGroupKind::Struct && Group.Struct != nullptr && Group.Address != nullptr)
    {
        for (TFieldIterator<FProperty> It(Group.Struct); It; ++It)
        {
            if (It->GetName() == Name)
            {
                Property = *It;
                break;
            }
        }
        Container = const_cast<uint8*>(Group.Address);
    }
    else if ((Group.Kind == EBlueprintVariableGroupKind::Array || Group.Kind == EBlueprintVariableGroupKind::Map) && Group.Property != nullptr && Group.Address != nullptr)
    {
        int32 Index = 0;
        if (!ParseContainerIndex(Name, Index))
        {
            Result.Error = FString::Printf(TEXT("%s is not a container element."), *Name);
            return Result;
        }

        if (Group.Kind == EBlueprintVariableGroupKind::Array)
        {
            const FArrayProperty* const ArrayProperty = CastField<FArrayProperty>(Group.Property);
            if (ArrayProperty && ArrayProperty->Inner)
            {
                FScriptArrayHelper Helper(ArrayProperty, Group.Address);
                if (Helper.IsValidIndex(Index))
                {
                    Property = ArrayProperty->Inner;
                    Container = Helper.GetRawPtr(Index);
                }
            }
        }
        else
        {
            const FMapProperty* const MapProperty = CastField<FMapProperty>(Group.Property);
            if (MapProperty && MapProperty->ValueProp)
            {
                FScriptMapHelper Helper(MapProperty, Group.Address);
                int32 DisplayIndex = 0;
                for (int32 ContainerIndex = 0; ContainerIndex < Helper.GetMaxIndex(); ++ContainerIndex)
                {
                    if (!Helper.IsValidIndex(ContainerIndex))
                    {
                        continue;
                    }
                    if (DisplayIndex++ == Index)
                    {
                        Property = MapProperty->ValueProp;
                        Container = Helper.GetValuePtr(ContainerIndex);
                        break;
                    }
                }
            }
        }
    }

    if (Property == nullptr || Container == nullptr)
    {
        Result.Error = FString::Printf(TEXT("%s is not a variable in this scope."), *Name);
        return Result;
    }
    FString EditError;
    if (!BlueprintInspectorVariableEdit::IsTextEditable(Property, EditError))
    {
        // The reason distinguishes a threading mistake from a property this debugger cannot edit.
        Result.Error = EditError;
        return Result;
    }

    FString OutValue;
    FString OutError;
    if (!BlueprintInspectorVariableEdit::Apply(Property, Container, const_cast<UObject*>(Group.Object), Value, OutValue, OutError))
    {
        Result.Error = OutError;
        return Result;
    }

    const FBlueprintVariable Updated = MakeVariableLocked(Property, Container, Group.Object);
    Result.bSuccess = true;
    Result.Value = Updated.Value;
    Result.Type = Updated.Type;
    Result.VariablesReference = Updated.VariablesReference;
    return Result;
}

int32 FBlueprintPauseSession::TakeNextReference(int32& Counter, TFunctionRef<bool(int32)> IsInUse)
{
    const int32 MaxReference = TNumericLimits<int32>::Max();
    if (Counter < 1)
    {
        Counter = 1;
    }

    while (IsInUse(Counter))
    {
        Counter = Counter == MaxReference ? 1 : Counter + 1;
    }

    const int32 Reference = Counter;
    Counter = Counter == MaxReference ? 1 : Counter + 1;
    return Reference;
}

int32 FBlueprintPauseSession::AllocateGroupLocked(const FBlueprintVariableGroup& Group) const
{
    FBlueprintVariableGroup Stamped = Group;
    Stamped.PauseGeneration = Generation;

    const int32 Reference = TakeNextReference(NextReference, [this](const int32 Candidate) { return Groups.Contains(Candidate); });
    Groups.Add(Reference, Stamped);
    return Reference;
}

FBlueprintVariable FBlueprintPauseSession::MakeVariableLocked(const FProperty* const Property, const uint8* const Container, const UObject* const Parent, const FString& Name, const EBlueprintVariableGroupAccess Access) const
{
    FBlueprintVariable Variable;
    Variable.Name = Name.IsEmpty() ? Property->GetName() : Name;
    Variable.Type = Property->GetCPPType();

    const void* const Value = Property->ContainerPtrToValuePtr<void>(Container);
    FString ValueText;
#if BI_ENGINE_AT_LEAST_5_4
    Property->ExportTextItem_Direct(ValueText, Value, nullptr, const_cast<UObject*>(Parent), PPF_PropertyWindow | PPF_BlueprintDebugView, nullptr);
#else
    Property->ExportTextItem(ValueText, Value, nullptr, const_cast<UObject*>(Parent), PPF_PropertyWindow | PPF_BlueprintDebugView, nullptr);
#endif
    Variable.Value = MoveTemp(ValueText);

    if (const FStructProperty* const StructProperty = CastField<FStructProperty>(Property))
    {
        if (StructProperty->Struct)
        {
            FBlueprintVariableGroup Group;
            Group.Struct = StructProperty->Struct;
            Group.Address = static_cast<const uint8*>(Value);
            Group.Object = Parent;
            Group.Access = Access;
            Variable.VariablesReference = AllocateGroupLocked(Group);
        }
    }
    else if (Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>())
    {
        FBlueprintVariableGroup Group;
        Group.Kind = Property->IsA<FArrayProperty>() ? EBlueprintVariableGroupKind::Array : (Property->IsA<FSetProperty>() ? EBlueprintVariableGroupKind::Set : EBlueprintVariableGroupKind::Map);
        Group.Property = Property;
        Group.Address = static_cast<const uint8*>(Value);
        Group.Object = Parent;
        Group.Access = Access;
        Variable.VariablesReference = AllocateGroupLocked(Group);
    }

    return Variable;
}

int32 FBlueprintPauseSession::FindLine(const int32 ClassHandle, const UFunction* const Function, const int64 Address) const
{
    FBlueprintSourceMap Map;
    if (!Registry.TryCopyMap(ClassHandle, Map))
    {
        return 1;
    }

    int32 Line = Map.FindLine(Function, Address);
    if (Line == 0)
    {
        // A stop can land on an operand rather than the statement's own byte; report the statement
        // that owns it instead of falling back to the top of the file.
        Line = Map.FindLineAtOrBefore(Function, Address);
    }

    return Line != 0 ? Line : 1;
}

int32 FBlueprintPauseSession::FindLineAtOrBefore(const int32 ClassHandle, const UFunction* const Function, const int64 Address) const
{
    FBlueprintSourceMap Map;
    if (!Registry.TryCopyMap(ClassHandle, Map))
    {
        return 1;
    }

    const int32 Line = Map.FindLineAtOrBefore(Function, Address);
    return Line != 0 ? Line : 1;
}
