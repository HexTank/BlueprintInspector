#include "BlueprintScriptDisassembler.h"
#include "BlueprintInspector.h"
#include "Misc/ScopeExit.h"
#include "UObject/Class.h"
#include "UObject/Package.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"

// ProcessOpcode is a recursive-descent stack machine. InDirection says where a decoded opcode's result
// lands:
//
//   Input:  an operand feeding the enclosing node, appended to Stack.Top()->InputNodes.
//   Output: an assignment target (l-value), stored in Stack.Top()->ReturnNode.
//   None:   a statement in the execution flow, reached later by walking OutputWires.
//
// Bytecode layouts are defined by Runtime/CoreUObject/Public/UObject/ScriptSerialization.h, and their
// runtime meaning by the exec* handlers in Private/UObject/ScriptCore.cpp.

namespace
{
// First-guess capacities for the decode walk and the small inline collections it uses.
constexpr int32 InitialStackReserve = 64;
constexpr int32 InlineInterfaceCandidateCount = 4;
constexpr int32 TransformComponentCount = 10;
constexpr int32 InlineBlueprintNodeQueueCapacity = 64;
constexpr int32 InitialInputNodeReserve = 4;

const FName NodeName_Cast(TEXT("Cast"));
const FName NodeName_MakeSet(TEXT("MakeSet"));
const FName NodeName_MakeMap(TEXT("MakeMap"));
const FName NodeName_Let(TEXT("Let"));
const FName NodeName_Return(TEXT("Return"));
const FName NodeName_MakeArray(TEXT("MakeArray"));
const FName NodeName_JumpIfNot(TEXT("JumpIfNot"));
const FName NodeName_Assert(TEXT("Assert"));
const FName NodeName_AddMulticastDelegate(TEXT("AddMulticastDelegate"));
const FName NodeName_RemoveMulticastDelegate(TEXT("RemoveMulticastDelegate"));
const FName NodeName_ClearMulticastDelegate(TEXT("ClearMulticastDelegate"));
const FName NodeName_BindDelegate(TEXT("BindDelegate"));
const FName NodeName_Sequence(TEXT("Sequence"));
const FName NodeName_PopExecutionFlowIfNot(TEXT("EX_PopExecutionFlowIfNot"));
const FName NodeName_Switch(TEXT("Switch"));
const FName NodeName_UnresolvedCall(TEXT("UnresolvedCall"));
} // namespace

FBlueprintScriptDisassembler::FBlueprintScriptDisassembler()
{
    Nodes.SetNum(NodePoolCapacity);
    NodeIterator = Nodes.GetData();
    TransformPool.SetNum(TransformPoolCapacity);
    VectorPool.SetNum(ConstantPoolCapacity);
    RotatorPool.SetNum(ConstantPoolCapacity);
#if BI_ENGINE_AT_LEAST_5
    Vector3fPool.SetNum(ConstantPoolCapacity);
#endif
}

bool FBlueprintScriptDisassembler::FunctionToNodes(const TWeakObjectPtr<const UFunction>& InFunction, const int64 InStartAddress)
{
    // Rebuild the pool in place. TArray owns the slot lifetime here: Reset() destroys the previous
    // graph's nodes and SetNum() constructs fresh ones, so no slot is ever destroyed twice and no
    // placement-new runs over a live object. The allocation is kept, so pointers into Nodes stay
    // stable for the whole run. Never grow Nodes after this: Stack, the wire lists and NodeIterator
    // all hold raw pointers into the array.
    Nodes.Reset();
    Nodes.SetNum(NodePoolCapacity);
    NodeIterator = Nodes.GetData();

    // The scratch state belongs to one run. A second run on this instance must start from an empty
    // graph, not append to the previous one or keep its exhausted-pool flag. DecodeDepth is reset here
    // too: it relies on DecodeOpcode's RAII scope balancing, and a skipped unwind must not poison the
    // next run.
    EntryPointNode = FBlueprintNode();
    OverflowNodes.Reset();
    ContextScratchNodes.Reset();
    ContextDepth = 0;
    bNodePoolExhaustedLogged = false;
    DecodeDepth = 0;
    PendingReceivers.Reset();
    PendingArrivalAddress = INDEX_NONE;
    PendingArrivalJumpOpcode = INDEX_NONE;
    PendingArrivalLanding = INDEX_NONE;
    TransformPoolCount = 0;
    bTransformPoolExhaustedLogged = false;
    VectorPoolCount = 0;
    bVectorPoolExhaustedLogged = false;
    RotatorPoolCount = 0;
    bRotatorPoolExhaustedLogged = false;
#if BI_ENGINE_AT_LEAST_5
    Vector3fPoolCount = 0;
    bVector3fPoolExhaustedLogged = false;
#endif
    Stack.Reset();

    // Clear the current function so SeekToOffset rebuilds the reader: a previous run can leave it at an end or
    // error offset, and the bytecode offsets are function-local.
    CurrentFunction.Reset();

    OpcodeLogger.BeginRun();

    if (!InFunction.IsValid())
    {
        return false;
    }

    if (!SeekToOffset(InFunction, InStartAddress))
    {
        return false;
    }

    Stack.Reserve(InitialStackReserve);
    EntryPointNode = FBlueprintNode(InFunction, InFunction->GetFName(), EX_Max);
    // Offset 0 is a real script address; the entry node is not a statement at it.
    EntryPointNode.ScriptIndex = INDEX_NONE;
    Stack.Push(&EntryPointNode);

    CurrentOpcodeScriptIndex = InStartAddress;
    OpcodeLogger.LogValue(TEXT("************************************************"));
    OpcodeLogger.LogValue(InFunction->GetName());
    OpcodeLogger.LogValue(TEXT("************************************************"));

    FunctionOwnerClass = InFunction->GetOwnerClass();
    DecodeOpcodeUntil(EX_EndOfScript, EDirection::None);
    FunctionOwnerClass = nullptr;

    // FinishFlow unwinds the stack before the terminator is read, and DecodeOpcodeUntil stops as soon as the
    // stack empties. Read the terminator here so the capture ends where the script does.
    if (Stack.Num() == 0 && !ScriptReader.IsError() && !ScriptReader.AtEnd())
    {
        const int64 NextOffset = ScriptReader.Tell();
        uint8 NextByte = 0;
        if (ScriptReader.ReadByte(NextByte) && static_cast<EExprToken>(NextByte) == EX_EndOfScript)
        {
            CurrentOpcodeScriptIndex = NextOffset;
            OpcodeLogger.LogOpcode(EX_EndOfScript, NextOffset);
        }
    }

    OpcodeLogger.LogValue(TEXT("************************************************"));
    OpcodeLogger.LogSummary(InFunction->GetName());
    return !ScriptReader.IsError();
}

bool FBlueprintScriptDisassembler::SeekToOffset(const TWeakObjectPtr<const UFunction>& InFunction, const int64 InAddress)
{
    if (!InFunction.IsValid())
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Disassembler: cannot seek in an invalid function"));
        return false;
    }

    if (CurrentFunction != InFunction)
    {
        // Bytecode offsets are function-local, so switching function invalidates the reader.
        ScriptReader.Rebind(InFunction->Script);
#if BI_ENGINE_AT_LEAST_5
        ScriptReader.SetUEVer(InFunction->GetOutermost()->GetLinkerPackageVersion());
#else
        ScriptReader.SetUE4Ver(InFunction->GetOutermost()->LinkerPackageVersion);
#endif
        // A pending arrival names an offset in the function we are leaving, so it cannot be adopted here.
        PendingArrivalAddress = INDEX_NONE;
        PendingArrivalJumpOpcode = INDEX_NONE;
        PendingArrivalLanding = INDEX_NONE;
    }

    // Addresses are int64 throughout the class. Checking here, rather than casting a uint64 back, keeps a
    // negative or INDEX_NONE address from wrapping into a huge unsigned value and seeking outside the
    // script; Seek() would otherwise clamp it and the walk would decode from the wrong offset.
    if (InAddress < 0 || InAddress > static_cast<int64>(ScriptReader.Length()))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Disassembler: start offset %lld is outside %s's script (%lld bytes)"), InAddress, InFunction.IsValid() ? *InFunction->GetName() : TEXT("<invalid function>"), static_cast<int64>(ScriptReader.Length()));
        CurrentFunction = InFunction;
        return false;
    }

    ScriptReader.Seek(InAddress);
    CurrentFunction = InFunction;
    return ScriptReader.Length() != 0;
}

EExprToken FBlueprintScriptDisassembler::DecodeOpcode(const EDirection InDirection)
{
    if (DecodeDepth >= MaxDecodeDepth)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Disassembler: opcode nesting exceeded %d levels at offset %lld"), MaxDecodeDepth, ScriptReader.Tell());
        ScriptReader.SetError();
        return EX_Nothing;
    }

    struct FDecodeDepthScope
    {
        int32& Depth;
        explicit FDecodeDepthScope(int32& InDepth) : Depth(InDepth)
        {
            ++Depth;
        }
        ~FDecodeDepthScope() { --Depth; }
    };
    const FDecodeDepthScope DepthScope(DecodeDepth);

    CurrentOpcodeScriptIndex = ScriptReader.Tell();
    uint8 OpcodeByte = 0;
    ScriptReader.ReadByte(OpcodeByte);
    EExprToken Opcode = static_cast<EExprToken>(OpcodeByte);
    if (ScriptReader.IsError())
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Disassembler: stream error at offset %lld"), ScriptReader.Tell());
        return EX_EndOfScript;
    }
    if (Stack.Num() == 0)
    {
        // The flow stack can be fully unwound by FinishFlow when no branch remains to follow.
        // Any bytes after that point are unreachable by the walker, so stop instead of crashing.
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Disassembler: empty node stack at offset %lld; stopping"), CurrentOpcodeScriptIndex);
        ScriptReader.SetError();
        return EX_EndOfScript;
    }

    OpcodeLogger.LogOpcode(Opcode, CurrentOpcodeScriptIndex);
    OpcodeLogger.Indent();
    Opcode = ProcessOpcode(Opcode, InDirection);
    OpcodeLogger.Unindent();
    return Opcode;
}

EExprToken FBlueprintScriptDisassembler::DecodeOpcodeUntil(const EExprToken InOpcode, const EDirection InDirection)
{
    EExprToken Opcode = EX_EndOfScript;
    while (!ScriptReader.IsError() && Stack.Num() > 0 && (Opcode = DecodeOpcode(InDirection)) != InOpcode)
    {
    }
    return Opcode;
}

static const UClass* ReceiverOwnerClass(const TOptional<FBlueprintNode::FNodeVariant>& InReceiver)
{
    if (!InReceiver.IsSet())
    {
        return nullptr;
    }

    if (const FContextProperty* const Property = InReceiver->TryGet<FContextProperty>())
    {
        if (Property->Property == nullptr)
        {
            return nullptr;
        }

        if (const FObjectProperty* const ObjectProperty = CastField<FObjectProperty>(Property->Property))
        {
            return ObjectProperty->PropertyClass;
        }

        if (const FInterfaceProperty* const InterfaceProperty = CastField<FInterfaceProperty>(Property->Property))
        {
            // FInterfaceProperty keeps its UClass private in 5.5, so resolve the interface class from the
            // property's C++ type name. A Blueprint interface named IBPI_ProbeA is generated as BPI_ProbeA_C.
            FString InterfaceType = InterfaceProperty->GetCPPType(nullptr, 0);
            int32 Open = INDEX_NONE;
            int32 Close = INDEX_NONE;
            if (InterfaceType.FindChar(TEXT('<'), Open) && InterfaceType.FindChar(TEXT('>'), Close) && Close > Open)
            {
                InterfaceType = InterfaceType.Mid(Open + 1, Close - Open - 1);
            }

            TArray<FString, TInlineAllocator<InlineInterfaceCandidateCount>> Candidates;
            Candidates.Add(InterfaceType);
            if (InterfaceType.StartsWith(TEXT("I")))
            {
                const FString WithoutI = InterfaceType.Mid(1);
                Candidates.Add(WithoutI + TEXT("_C"));
                Candidates.Add(WithoutI);
            }
            Candidates.Add(InterfaceType + TEXT("_C"));

            for (TObjectIterator<UClass> It; It; ++It)
            {
                const UClass* const Class = *It;
                // Name-only matching would take a plain class that happens to share the interface's
                // generated name, and the interface resolution is what decides the receiver's class.
                if (!Class->HasAnyClassFlags(CLASS_Interface))
                {
                    continue;
                }
                for (const FString& Candidate : Candidates)
                {
                    if (Class->GetName() == Candidate)
                    {
                        return Class;
                    }
                }
            }
        }

        return nullptr;
    }

    if (const UObject* const* const Object = InReceiver->TryGet<const UObject*>())
    {
        const UObject* const Receiver = *Object;
        if (Receiver == nullptr)
        {
            return nullptr;
        }
        return Receiver->IsA<UClass>() ? static_cast<const UClass*>(Receiver) : Receiver->GetClass();
    }

    return nullptr;
}

static void LiftToOperand(FBlueprintNode& InParent, FBlueprintNode* InChild)
{
    // The overflow path can hand back the parent itself, and it does not add an output wire when the
    // pool is exhausted. Only lift a child that is actually the last wire, and never link a node into
    // itself: the visualizer would recurse forever on that cycle.
    if (InChild == nullptr || InChild == &InParent)
    {
        return;
    }

    if (InParent.OutputWires.Num() > 0)
    {
        if (FBlueprintNode** const Wire = InParent.OutputWires.Last().TryGet<FBlueprintNode*>())
        {
            if (*Wire == InChild)
            {
                InParent.OutputWires.Pop();
            }
        }
    }

    InParent.InputNodes.Emplace_GetRef().Emplace<FBlueprintNode*>(InChild);
}

template <typename T, typename U, bool bLog>
void FBlueprintScriptDisassembler::EmitOperand()
{
    T Value{};
    ScriptReader << Value;
    Stack.Top()->InputNodes.Emplace_GetRef().Emplace<U>(Value);
    if constexpr (bLog)
    {
        OpcodeLogger.LogValue(Value);
    }
}

template <typename T>
void FBlueprintScriptDisassembler::EmitScriptPointerOperand()
{
    T Value{};
    ScriptReader << Value;
    if (ScriptReader.IsError())
    {
        return;
    }

    if (const T* const Stored = StoreConstant(Value))
    {
        Stack.Top()->InputNodes.Emplace_GetRef().Emplace<const T*>(Stored);
    }
    else
    {
        ScriptReader.SetError();
    }
}

const FVector* FBlueprintScriptDisassembler::StoreConstant(const FVector& Value)
{
    if (VectorPoolCount >= VectorPool.Num())
    {
        if (!bVectorPoolExhaustedLogged)
        {
            bVectorPoolExhaustedLogged = true;
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Disassembler: vector constant pool exhausted (%d slots); the constant at offset %lld is consumed but not linked"), VectorPool.Num(), CurrentOpcodeScriptIndex);
        }
        return nullptr;
    }

    const int32 Index = VectorPoolCount++;
    VectorPool[Index] = Value;
    return &VectorPool[Index];
}

const FRotator* FBlueprintScriptDisassembler::StoreConstant(const FRotator& Value)
{
    if (RotatorPoolCount >= RotatorPool.Num())
    {
        if (!bRotatorPoolExhaustedLogged)
        {
            bRotatorPoolExhaustedLogged = true;
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Disassembler: rotator constant pool exhausted (%d slots); the constant at offset %lld is consumed but not linked"), RotatorPool.Num(), CurrentOpcodeScriptIndex);
        }
        return nullptr;
    }

    const int32 Index = RotatorPoolCount++;
    RotatorPool[Index] = Value;
    return &RotatorPool[Index];
}

#if BI_ENGINE_AT_LEAST_5
const BI_FVector3f* FBlueprintScriptDisassembler::StoreConstant(const BI_FVector3f& Value)
{
    if (Vector3fPoolCount >= Vector3fPool.Num())
    {
        if (!bVector3fPoolExhaustedLogged)
        {
            bVector3fPoolExhaustedLogged = true;
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Disassembler: FVector3f constant pool exhausted (%d slots); the constant at offset %lld is consumed but not linked"), Vector3fPool.Num(), CurrentOpcodeScriptIndex);
        }
        return nullptr;
    }

    const int32 Index = Vector3fPoolCount++;
    Vector3fPool[Index] = Value;
    return &Vector3fPool[Index];
}
#endif

EExprToken FBlueprintScriptDisassembler::ProcessOpcode(const EExprToken InOpcode, const EDirection InDirection)
{
    switch (InOpcode)
    {
#if BI_ENGINE_AT_LEAST_5
    // Bytecode: conversion type (uint8), then the object being converted. CST_* values live in Script.h.
    case EX_Cast:
    {
        uint8 ConversionType = 0;
        ScriptReader << ConversionType;
        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(NodeName_Cast, InOpcode);
        NewNode->InputNodes.Emplace_GetRef().Emplace<uint64>(ConversionType);
        DecodeOpcode(EDirection::Input);
        if (InDirection == EDirection::Input && Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
#endif
#if !BI_ENGINE_AT_LEAST_5
    // 4.25's name for the same opcode; the conversion type is a 4.25 ECastToken byte.
    case EX_PrimitiveCast:
    {
        uint8 ConversionType = 0;
        ScriptReader << ConversionType;
        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(NodeName_Cast, InOpcode);
        NewNode->InputNodes.Emplace_GetRef().Emplace<uint64>(ConversionType);
        DecodeOpcode(EDirection::Input);
        if (InDirection == EDirection::Input && Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
#endif
    case EX_SetSet:
    {
        FBlueprintNode* const NewNode = PushNewNode(NodeName_MakeSet, InOpcode);
        DecodeOpcode(EDirection::Output);
        int32 NumberOfValues = 0;
        ScriptReader << NumberOfValues;
        DecodeOpcodeUntil(EX_EndSet, EDirection::Input);
        if (const FSetProperty* const SetProperty = CastField<FSetProperty>(NewNode->ReturnNode.Property))
        {
            NewNode->ContainerLiteral = FBlueprintContainerLiteral{EBlueprintContainerKind::Set, SetProperty->ElementProp, nullptr, NumberOfValues};
        }
        break;
    }
    case EX_SetConst:
    {
        FProperty* InnerProp = nullptr;
        int32 NumberOfValues = 0;
        ScriptReader << InnerProp << NumberOfValues;

        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(NodeName_MakeSet, InOpcode);
        NewNode->ContainerLiteral = FBlueprintContainerLiteral{EBlueprintContainerKind::Set, InnerProp, nullptr, NumberOfValues};
        DecodeOpcodeUntil(EX_EndSetConst, EDirection::Input);
        if (Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
    case EX_SetMap:
    {
        FBlueprintNode* const NewNode = PushNewNode(NodeName_MakeMap, InOpcode);
        DecodeOpcode(EDirection::Output);

        int32 NumberOfKeyValuesPairs = 0;
        ScriptReader << NumberOfKeyValuesPairs;
        DecodeOpcodeUntil(EX_EndMap, EDirection::Input);
        if (const FMapProperty* const MapProperty = CastField<FMapProperty>(NewNode->ReturnNode.Property))
        {
            NewNode->ContainerLiteral = FBlueprintContainerLiteral{
                EBlueprintContainerKind::Map, MapProperty->KeyProp, MapProperty->ValueProp, NumberOfKeyValuesPairs};
        }
        break;
    }

    case EX_MapConst:
    {
        FProperty* KeyProp = nullptr;
        FProperty* ValProp = nullptr;
        int32 NumberOfKeyValuePairs = 0;
        ScriptReader << KeyProp << ValProp << NumberOfKeyValuePairs;

        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(NodeName_MakeMap, InOpcode);
        NewNode->ContainerLiteral = FBlueprintContainerLiteral{EBlueprintContainerKind::Map, KeyProp, ValProp, NumberOfKeyValuePairs};
        DecodeOpcodeUntil(EX_EndMapConst, EDirection::Input);
        if (Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
    case EX_MetaCast:
    case EX_DynamicCast:
    case EX_ObjToInterfaceCast:
    case EX_CrossInterfaceCast:
    case EX_InterfaceToObjCast:
    {
        UClass* ObjectClass = nullptr;
        ScriptReader << ObjectClass;
        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(NodeName_Cast, InOpcode);
        NewNode->InputNodes.Emplace_GetRef().Emplace<const UObject*>(ObjectClass);
        DecodeOpcode(EDirection::Input);
        if (InDirection == EDirection::Input && Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
    case EX_Let:
    case EX_LetObj:
    case EX_LetBool:
    case EX_LetDelegate:
    case EX_LetMulticastDelegate:
    {
        // EX_Let also carries a trailing FProperty* for the destination.
        if (InOpcode == EX_Let)
        {
            const FProperty* Property = nullptr;
            ScriptReader << Property;
        }
        PushNewNode(NodeName_Let, InOpcode);
        DecodeOpcode(EDirection::Output);
        DecodeOpcode(EDirection::Input);
        break;
    }

    case EX_LetWeakObjPtr:
    {
        PushNewNode(NodeName_Let, InOpcode);
        DecodeOpcode(EDirection::Output);
        DecodeOpcode(EDirection::Input);
        break;
    }

    // A let whose destination is a persistent-frame property, which is serialised inline as a property
    // pointer rather than encoded as a variable expression.
    case EX_LetValueOnPersistentFrame:
    {
        FProperty* Property = nullptr;
        ScriptReader << Property;
        FBlueprintNode* const NewNode = PushNewNode(NodeName_Let, InOpcode);
        NewNode->ReturnNode = FContextProperty{nullptr, Property};
        DecodeOpcode(EDirection::Input);
        break;
    }
    // Bytecode: the struct member property, then the struct expression. The member is kept separately so
    // the visualizer can render it as "Struct.Member".
    case EX_StructMemberContext:
    {
        FProperty* Property = nullptr;
        ScriptReader << Property;
        DecodeOpcode(InDirection);
        if (Stack.Num() == 0)
        {
            ScriptReader.SetError();
            break;
        }
        if (InDirection == EDirection::Input)
        {
            Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FContextProperty>(FContextProperty{nullptr, Property});
        }
        else if (InDirection == EDirection::Output)
        {
            Stack.Top()->ReturnNode = FContextProperty{nullptr, Property};
        }
        break;
    }
    // Bytecode: the UFunction* pointer, then the parameters up to EX_EndFunctionParms. The VM calls
    // ProcessLocalFunction, which runs the callee's own script and returns here; nothing in the caller's
    // bytecode describes the callee's body, so the walk stops at the call.
    case EX_CallMath:
    case EX_FinalFunction:
    case EX_LocalFinalFunction:
    {
        const UFunction* Function = nullptr;
        ScriptReader << Function;
        if (Function == nullptr)
        {
            ScriptReader.SetError();
            break;
        }
        FBlueprintNode* NewNode = DecodeFunctionData(InOpcode, InDirection, Function);
        if (NewNode != nullptr && Function->HasAllFunctionFlags(FUNC_Static))
        {
            NewNode->Name = *FString::Printf(TEXT("U%s::%s"), *Function->GetOuter()->GetName(), *Function->GetName());
        }
        break;
    }
    // Bytecode: the function name (not a UFunction*), then the parameters. The target is resolved against
    // FunctionOwnerClass, which EX_Context temporarily narrows to the receiver's class so the lookup finds
    // overrides rather than only the declaring class's version.
    case EX_VirtualFunction:
    case EX_LocalVirtualFunction:
    {
        FScriptName FunctionName;
        ScriptReader << FunctionName;
        const FName ResolvedName = BI_ScriptNameToName(FunctionName);
        // EX_Context narrows FunctionOwnerClass, but a virtual call can be reached with no receiver
        // context at all; GetOwnerClass() is allowed to return null.
        const UFunction* Function = FunctionOwnerClass.IsValid() ? FunctionOwnerClass->FindFunctionByName(ResolvedName) : nullptr;
        if (Function == nullptr)
        {
            // Interface message calls name a function on the interface class, not on the receiver's
            // class. Prefer an interface the receiver actually implements; two interfaces can declare the
            // same function name.
            if (FunctionOwnerClass.IsValid())
            {
                for (TObjectIterator<UClass> It; It; ++It)
                {
                    const UClass* const Class = *It;
                    if (Class->HasAnyClassFlags(CLASS_Interface) && FunctionOwnerClass->ImplementsInterface(Class))
                    {
                        if (const UFunction* const InterfaceFunction = Class->FindFunctionByName(ResolvedName))
                        {
                            Function = InterfaceFunction;
                            break;
                        }
                    }
                }
            }
            if (Function == nullptr)
            {
                for (TObjectIterator<UClass> It; It; ++It)
                {
                    const UClass* const Class = *It;
                    if (Class->HasAnyClassFlags(CLASS_Interface))
                    {
                        if (const UFunction* const InterfaceFunction = Class->FindFunctionByName(ResolvedName))
                        {
                            Function = InterfaceFunction;
                            break;
                        }
                    }
                }
            }
        }
        if (Function == nullptr)
        {
            // A miss here is not benign: the call creates no node, so the parameter list that follows is never
            // consumed by it and decodes as the enclosing function's statements. Naming what was looked for,
            // and against which class, is what makes that visible.
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Unresolved virtual call '%s' against '%s'"), *ResolvedName.ToString(), FunctionOwnerClass.IsValid() ? *FunctionOwnerClass->GetName() : TEXT("<no owner class>"));
        }
        (void)DecodeFunctionData(InOpcode, InDirection, Function);
        break;
    }

    case EX_ComputedJump:
        // Bytecode: the jump target expression. The destination is computed at runtime and is not
        // encoded, so the walk cannot follow it. Decode the expression and terminate the linear flow
        // instead of treating the unreachable bytes that follow as the next statement.
        DecodeOpcode(EDirection::Input);
        UE_LOG(LogBlueprintInspector, Log, TEXT("Disassembler: computed jump at offset %lld; its target is not statically recoverable"), CurrentOpcodeScriptIndex);
        return FinishFlow(InOpcode);
    case EX_Jump:
    {
        CodeSkipSizeType Address = 0;
        ScriptReader << Address;
        // execJump assigns Code = &Script[Offset], so the operand is the landing the VM will hold.
        Stack.Top()->WiresToProcess.Push(FProcessWireData{CurrentFunction, SanitiseAddress(Address), CurrentOpcodeScriptIndex, static_cast<int64>(Address)});
        return FinishFlow(InOpcode);
    }
    case EX_LocalVariable:
    case EX_DefaultVariable:
    case EX_InstanceVariable:
    case EX_LocalOutVariable:
#if BI_ENGINE_AT_LEAST_5_1
    case EX_ClassSparseDataVariable:
#endif
    {
        FProperty* Property = nullptr;
        ScriptReader << Property;
        if (Property == nullptr)
        {
            break;
        }

        OpcodeLogger.LogValue(Property);

        if (InDirection == EDirection::Input)
        {
            Stack.Top()->InputNodes.AddDefaulted_GetRef().Emplace<FContextProperty>(FContextProperty{nullptr, Property});
        }
        else if (InDirection == EDirection::Output)
        {
            Stack.Top()->ReturnNode = FContextProperty{nullptr, Property};
        }
        break;
    }
    case EX_InterfaceContext:
    {
        DecodeOpcode(InDirection);
        break;
    }

#if BI_ENGINE_AT_LEAST_5_3
    case EX_NothingInt32:
    {
        int32 Value = 0;
        ScriptReader << Value;
        OpcodeLogger.LogValue(Value);
        if (InDirection == EDirection::Input)
        {
            Stack.Top()->InputNodes.Emplace_GetRef().Emplace<int64>(Value);
        }
        break;
    }
#endif

    case EX_Return:
    {
        // KismetCompilerVMBackend::EmitReturn always writes an expression after EX_Return: EX_Nothing for a
        // void function, or EX_LocalOutVariable plus the return property otherwise. Decoding onto a real
        // node rather than a scratch one keeps that value so the visualizer can print "return X;".
        PushNewNode(NodeName_Return, InOpcode);
        DecodeOpcode(EDirection::Input);
        return FinishFlow(InOpcode);
    }

    case EX_CallMulticastDelegate:
    {
        UStruct* StackNode = nullptr;
        ScriptReader << StackNode;
        OpcodeLogger.LogValue(static_cast<const UObject*>(StackNode));

        const FName SignatureName = StackNode ? StackNode->GetFName() : FName(TEXT("CallMulticastDelegate"));
        PushNewNode(SignatureName, InOpcode);
        DecodeOpcode(EDirection::Input);
        DecodeOpcodeUntil(EX_EndFunctionParms, EDirection::Input);
        break;
    }

    // Bytecode: the object expression, a code offset for NULL expressions, the FField* being addressed,
    // then the member expression proper. This is both "Object.Member" reads and writes.
    case EX_ClassContext:
    case EX_Context:
    case EX_Context_FailSilent:
    {
        // The member lookup below runs against the receiver's class, but this context may itself sit
        // inside another context's member expression. Save the enclosing lookup context and restore
        // it afterwards; restoring from the top-level function would leak the top-level function's class
        // into a nested context (and dereference a weak pointer that may have been collected).
        const TWeakObjectPtr<const UClass> SavedOwnerClass = FunctionOwnerClass;
        ON_SCOPE_EXIT
        {
            FunctionOwnerClass = SavedOwnerClass;
        };

        // The object expression is decoded onto a scratch parent. It must outlive this case: every
        // node created for the expression keeps an InputWire to it, and FinishFlow's already-visited
        // walk follows InputWires later. Each nesting level owns its scratch node: a nested context
        // (A.B.C) must not reset the parent its enclosing context still has on Stack.
        const int32 Level = ContextDepth++;
        ON_SCOPE_EXIT
        {
            --ContextDepth;
        };
        if (!ContextScratchNodes.IsValidIndex(Level))
        {
            ContextScratchNodes.SetNum(Level + 1);
        }
        if (!ContextScratchNodes[Level].IsValid())
        {
            ContextScratchNodes[Level] = MakeUnique<FBlueprintNode>();
        }
        FBlueprintNode* const Scratch = ContextScratchNodes[Level].Get();
        *Scratch = FBlueprintNode();
        // Never let the scratch parent match a real jump landing in FinishFlow's visited check.
        Scratch->ScriptIndex = INDEX_NONE;
        const int32 StackDepth = Stack.Num();
        Stack.Push(Scratch);
        DecodeOpcode(EDirection::Input);
        // An expression that left nodes on the stack would make a single Pop remove the wrong entry;
        // unwind to the depth this context was entered with.
        while (Stack.Num() > StackDepth)
        {
            Stack.Pop(BI_POP_NO_SHRINK);
        }

        // One receiver slot per nesting level: this context's member expression may itself be a context,
        // which parks its own receiver while this one is still waiting to be attached. The slot is popped
        // when this case ends, whether or not a node consumed it.
        const int32 ReceiverDepth = PendingReceivers.Num();
        PendingReceivers.AddDefaulted();
        ON_SCOPE_EXIT
        {
            PendingReceivers.SetNum(ReceiverDepth);
        };

        if (Scratch->InputNodes.Num() != 0)
        {
            PendingReceivers.Last() = Scratch->InputNodes.Top();
        }

        CodeSkipSizeType Offset = 0;
        ScriptReader << Offset;
        FField* Field = nullptr;
        ScriptReader << Field;

        if (const UClass* const ReceiverClass = ReceiverOwnerClass(PendingReceivers.Last()))
        {
            FunctionOwnerClass = ReceiverClass;
        }

        DecodeOpcode(InDirection);

        if (Stack.Num() == 0)
        {
            ScriptReader.SetError();
            break;
        }

        if (InDirection == EDirection::Output)
        {
            const TOptional<FBlueprintNode::FNodeVariant>& Parked = PendingReceivers.Last();
            if (const FContextProperty* const Receiver = Parked.IsSet() ? Parked->TryGet<FContextProperty>() : nullptr)
            {
                if (Receiver->Property != nullptr && Stack.Top()->ReturnNode.Property != nullptr)
                {
                    Stack.Top()->ReturnNode.Context = Receiver->Property;
                }
            }
        }

        // The member was a plain property read, which creates no node, so whatever is still parked is
        // dropped with the slot rather than attaching itself to a later node.
        break;
    }
    case EX_IntConst:
        EmitOperand<int32, int64>();
        break;
    case EX_Int64Const:
        EmitOperand<int64>();
        break;
    case EX_UInt64Const:
        EmitOperand<uint64>();
        break;
    case EX_SkipOffsetConst:
    {
        CodeSkipSizeType ConstValue = 0;
        ScriptReader << ConstValue;
        OpcodeLogger.LogValue(static_cast<uint32>(ConstValue));
        break;
    }
    case EX_FloatConst:
        EmitOperand<float, double>();
        break;
#if BI_ENGINE_AT_LEAST_5
    case EX_DoubleConst:
        EmitOperand<double>();
        break;
#endif
    // Bytecode: a raw NUL-terminated ANSI string, not an FString, so it has to be measured and the cursor
    // advanced past the terminator by hand rather than read with operator<<. The string is stored as an
    // FName input so the visualizer quotes it.
    case EX_StringConst:
    {
        const int64 Remaining = static_cast<int64>(ScriptReader.Length()) - ScriptReader.Tell();
        const ANSICHAR* String = ScriptReader.GetCurrentScriptPointer<ANSICHAR>();
        if (String == nullptr)
        {
            ScriptReader.SetError();
            break;
        }
        int64 StringByteLength = 0;
        while (StringByteLength < Remaining && String[StringByteLength] != '\0')
        {
            ++StringByteLength;
        }
        if (StringByteLength == Remaining)
        {
            ScriptReader.SetError();
            break;
        }
        FString ConstValue{StringCast<TCHAR>(String).Get()};
        Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FName>(ConstValue);

        OpcodeLogger.LogValue(ConstValue);
        if (!SeekToOffset(CurrentFunction, ScriptReader.Tell() + StringByteLength + 1))
        {
            // A seek the bounds check refuses leaves the reader where it is, so the string's bytes would
            // decode as opcodes. Stop instead of mis-reading the rest of the function.
            ScriptReader.SetError();
            break;
        }
        break;
    }
    case EX_UnicodeStringConst:
    {
        const int64 RemainingBytes = static_cast<int64>(ScriptReader.Length()) - ScriptReader.Tell();
        const int64 RemainingWide = RemainingBytes / static_cast<int64>(sizeof(WIDECHAR));
        const WIDECHAR* String = ScriptReader.GetCurrentScriptPointer<WIDECHAR>();
        if (String == nullptr)
        {
            ScriptReader.SetError();
            break;
        }

        // The operand is not aligned.
        int64 StringLength = 0;
        WIDECHAR Char = 0;
        while (StringLength < RemainingWide)
        {
            FMemory::Memcpy(&Char, reinterpret_cast<const uint8*>(String) + StringLength * sizeof(WIDECHAR), sizeof(WIDECHAR));
            if (Char == TEXT('\0'))
            {
                break;
            }
            ++StringLength;
        }
        if (StringLength == RemainingWide || StringLength > MAX_int32 - 1)
        {
            ScriptReader.SetError();
            break;
        }
        TArray<WIDECHAR> Buffer;
        Buffer.SetNumZeroed(static_cast<int32>(StringLength) + 1);
        if (StringLength > 0)
        {
            FMemory::Memcpy(Buffer.GetData(), reinterpret_cast<const uint8*>(String), StringLength * static_cast<int64>(sizeof(WIDECHAR)));
        }
        FString ConstValue{Buffer.GetData()};
        Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FName>(ConstValue);

        OpcodeLogger.LogValue(ConstValue);
        if (!SeekToOffset(CurrentFunction, ScriptReader.Tell() + (StringLength + 1) * static_cast<int64>(sizeof(WIDECHAR))))
        {
            ScriptReader.SetError();
            break;
        }
        break;
    }
    case EX_TextConst:
    {
        EBlueprintTextLiteralType TextLiteralType = EBlueprintTextLiteralType::Empty;
        ScriptReader << reinterpret_cast<uint8&>(TextLiteralType);

        switch (TextLiteralType)
        {
        case EBlueprintTextLiteralType::Empty:
            if (InDirection == EDirection::Input)
            {
                Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FName>(TEXT(""));
            }
            break;

        case EBlueprintTextLiteralType::LocalizedText:
        {
            FString SourceString;
            FString KeyString;
            FString Namespace;

            ScriptReader << SourceString;
            ScriptReader << KeyString;
            ScriptReader << Namespace;
            OpcodeLogger.LogValue(SourceString);
            OpcodeLogger.LogValue(KeyString);
            OpcodeLogger.LogValue(Namespace);

            if (InDirection == EDirection::Input)
            {
                Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FName>(FName(*SourceString));
            }
        }
        break;

        case EBlueprintTextLiteralType::InvariantText:
        case EBlueprintTextLiteralType::LiteralString:
            EmitOperand<FString, FName>();
            break;

        case EBlueprintTextLiteralType::StringTableEntry:
        {
            UObject* Object = nullptr;
            FString TableIdString;
            FString KeyString;
            ScriptReader << Object << TableIdString << KeyString;
            OpcodeLogger.LogValue(Object);
            OpcodeLogger.LogValue(TableIdString);
            OpcodeLogger.LogValue(KeyString);

            if (InDirection == EDirection::Input)
            {
                Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FName>(FName(*KeyString));
            }
        }
        break;

        default:
            UE_LOG(LogBlueprintInspector, Error, TEXT("Unknown EBlueprintTextLiteralType %d"), static_cast<int32>(TextLiteralType));
            ScriptReader.SetError();
            break;
        }
        break;
    }
#if BI_ENGINE_AT_LEAST_5
    case EX_PropertyConst:
    {
        FProperty* Property = nullptr;
        ScriptReader << Property;
        if (Property == nullptr)
        {
            break;
        }
        OpcodeLogger.LogValue(Property);
        Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FContextProperty>(FContextProperty{nullptr, Property});
        break;
    }
#endif
    case EX_ObjectConst:
        EmitOperand<UObject*, const UObject*>();
        break;
    case EX_SoftObjectConst:
    case EX_FieldPathConst:
        EmitOperand<FString, FName>();
        break;
    case EX_NameConst:
    {
        FScriptName ScriptName;
        ScriptReader << ScriptName;
        Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FName>(BI_ScriptNameToName(ScriptName));
        OpcodeLogger.LogValue(ScriptName);
        break;
    }
    case EX_RotationConst:
        EmitScriptPointerOperand<FRotator>();
        break;
    case EX_VectorConst:
        EmitScriptPointerOperand<FVector>();
        break;
#if BI_ENGINE_AT_LEAST_5
    case EX_Vector3fConst:
        EmitScriptPointerOperand<BI_FVector3f>();
        break;
#endif
    case EX_TransformConst:
    {
        // The VM reads this constant one component at a time (execTransformConst): ten floats before
        // LWC, ten doubles from UE 5.0. Serialising an FTransform instead goes through the engine's
        // math operators, which switch on the archive's UE version; this reader carries bytecode, not a
        // package, so it has no version and they take the pre-5.0 float path. A 5.x transform then
        // consumed 40 bytes instead of 80, the cursor landed mid-constant, and the rest of the function
        // decoded as garbage (a stray -1 read as a property pointer crashed the process).
        FTransform Value;
#if BI_ENGINE_AT_LEAST_5
        double Parts[TransformComponentCount] = {};
        for (double& Part : Parts)
        {
            ScriptReader << Part;
        }
#else
        float Parts[TransformComponentCount] = {};
        for (float& Part : Parts)
        {
            ScriptReader << Part;
        }
#endif
        Value = FTransform(FQuat(Parts[0], Parts[1], Parts[2], Parts[3]), FVector(Parts[4], Parts[5], Parts[6]), FVector(Parts[7], Parts[8], Parts[9]));

        if (InDirection == EDirection::Input && TransformPoolCount < TransformPool.Num())
        {
            TransformPool[TransformPoolCount] = Value;
            const FTransform& Stored = TransformPool[TransformPoolCount++];
            Stack.Top()->InputNodes.Emplace_GetRef().Emplace<const FTransform*>(&Stored);
        }
        else if (InDirection == EDirection::Input)
        {
            // A dropped operand would render a statement without its constant, so mark the run
            // incomplete rather than only logging it.
            ScriptReader.SetError();
            if (!bTransformPoolExhaustedLogged)
            {
                bTransformPoolExhaustedLogged = true;
                UE_LOG(LogBlueprintInspector, Warning, TEXT("Disassembler: transform constant pool exhausted (%d slots); the constant at offset %lld is consumed but not linked; the render is marked incomplete"), TransformPool.Num(), CurrentOpcodeScriptIndex);
            }
        }
        break;
    }
    case EX_StructConst:
    {
        UScriptStruct* Struct = nullptr;
        int32 SerializedSize = 0;
        ScriptReader << Struct << SerializedSize;
        if (Struct == nullptr)
        {
            ScriptReader.SetError();
            break;
        }

        FBlueprintNode* CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(Struct->GetFName(), InOpcode);
        DecodeOpcodeUntil(EX_EndStructConst, EDirection::Input);
        if (Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
    case EX_SetArray:
    {
        FBlueprintNode* const CurrentNode = PushNewNode(NodeName_MakeArray, InOpcode);
        DecodeOpcode(EDirection::Output);

        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(NodeName_MakeArray, InOpcode);
        DecodeOpcodeUntil(EX_EndArray, EDirection::Input);
        if (const FArrayProperty* const ArrayProperty = CastField<FArrayProperty>(CurrentNode->ReturnNode.Property))
        {
            NewNode->ContainerLiteral = FBlueprintContainerLiteral{
                EBlueprintContainerKind::Array, ArrayProperty->Inner, nullptr, NewNode->InputNodes.Num()};
        }
        if (Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
    case EX_ArrayConst:
    {
        FProperty* InnerProp = nullptr;
        int32 NumberOfValues = 0;
        ScriptReader << InnerProp << NumberOfValues;

        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(NodeName_MakeArray, InOpcode);
        NewNode->ContainerLiteral = FBlueprintContainerLiteral{EBlueprintContainerKind::Array, InnerProp, nullptr, NumberOfValues};
        DecodeOpcodeUntil(EX_EndArrayConst, EDirection::Input);
        if (Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
#if BI_ENGINE_AT_LEAST_5_3
    case EX_BitFieldConst:
    {
        FProperty* BitProperty = nullptr;
        uint8 ConstValue = 0;
        ScriptReader << BitProperty << ConstValue;
        if (InDirection == EDirection::Input)
        {
            // The literal is the bit's value. The property is only the coercion target the compiler
            // used; keeping it beside the value would make the visualizer print an extra argument.
            Stack.Top()->InputNodes.Emplace_GetRef().Emplace<EExprToken>(ConstValue != 0 ? EX_True : EX_False);
        }
        else if (InDirection == EDirection::Output)
        {
            Stack.Top()->ReturnNode = FContextProperty{nullptr, BitProperty};
        }
        break;
    }
#endif
    case EX_ByteConst:
        EmitOperand<uint8, uint64, /*bLog=*/false>();
        break;
    case EX_IntConstByte:
        EmitOperand<uint8, int64, /*bLog=*/false>();
        break;
    case EX_JumpIfNot:
    {
        // The jump's own offset has to be taken before the condition is decoded: DecodeOpcode resets
        // CurrentOpcodeScriptIndex for every nested opcode, so reading it after the condition would record
        // the condition's last opcode instead of the branch the VM dispatches.
        const int64 JumpScriptIndex = CurrentOpcodeScriptIndex;
        CodeSkipSizeType Address = 0;
        ScriptReader << Address;
        OpcodeLogger.LogValue(static_cast<uint32>(Address));

        FBlueprintNode* const NewNode = PushNewNode(NodeName_JumpIfNot, InOpcode);
        DecodeOpcode(EDirection::Input);

        const int64 TruePath = ScriptReader.Tell();
        const int64 FalsePath = Address;
        // Taking the branch sets Code = &Script[Offset]; falling through leaves Code after the operand, so
        // TruePath is the fallthrough landing and FalsePath the taken one.
        NewNode->WiresToProcess.Push(FProcessWireData{CurrentFunction, SanitiseAddress(TruePath), JumpScriptIndex, TruePath});
        NewNode->WiresToProcess.Push(FProcessWireData{CurrentFunction, SanitiseAddress(FalsePath), JumpScriptIndex, FalsePath});
        FinishFlow(InOpcode);
        break;
    }
    case EX_Assert:
    {
        uint16 LineNumber = 0;
        uint8 InDebugMode = 0;
        ScriptReader << LineNumber << InDebugMode;
        PushNewNode(NodeName_Assert, InOpcode);
        DecodeOpcode(EDirection::Input);
        // exec_Assert does not transfer control: on success the VM continues with the next opcode, so
        // the walk must stay on the current flow instead of treating the assert as a terminator.
        break;
    }
    case EX_Skip:
    {
        CodeSkipSizeType Offset = 0;
        ScriptReader << Offset;
        // exec_Skip advances Code past the operand by Offset; mirror it or the skipped bytes decode as
        // opcodes and the rest of the function is misaligned. The operand is unvalidated bytecode, so
        // the seek can be refused: stop rather than decode the following bytes at the wrong offset.
        const int64 SkipTarget = ScriptReader.Tell() + Offset;
        if (Offset < 0 || SkipTarget < ScriptReader.Tell() || !SeekToOffset(CurrentFunction, SkipTarget))
        {
            ScriptReader.SetError();
            return EX_EndOfScript;
        }
        break;
    }
    case EX_InstanceDelegate:
    {
        FScriptName ScriptName;
        ScriptReader << ScriptName;
        Stack.Top()->InputNodes.Emplace_GetRef().Emplace<FDelegateLiteral>(FDelegateLiteral{BI_ScriptNameToName(ScriptName)});
        break;
    }
    case EX_AddMulticastDelegate:
    {
        PushNewNode(NodeName_AddMulticastDelegate, InOpcode);
        DecodeOpcode(EDirection::Output);
        DecodeOpcode(EDirection::Input);
        break;
    }
    case EX_RemoveMulticastDelegate:
    {
        PushNewNode(NodeName_RemoveMulticastDelegate, InOpcode);
        DecodeOpcode(EDirection::Output);
        DecodeOpcode(EDirection::Input);
        break;
    }
    case EX_ClearMulticastDelegate:
    {
        PushNewNode(NodeName_ClearMulticastDelegate, InOpcode);
        DecodeOpcode(EDirection::Output);
        break;
    }
    case EX_BindDelegate:
    {
        FScriptName FuncName;
        ScriptReader << FuncName;

        FBlueprintNode* const NewNode = PushNewNode(NodeName_BindDelegate, InOpcode);
        DecodeOpcode(EDirection::Output);
        DecodeOpcode(EDirection::Input);

        // Pushed last so it renders as the trailing Bind() argument.
        NewNode->InputNodes.Emplace_GetRef().Emplace<FName>(BI_ScriptNameToName(FuncName));

        break;
    }
    case EX_PushExecutionFlow:
    {
        CodeSkipSizeType Address = 0;
        ScriptReader << Address;
        OpcodeLogger.LogValue(static_cast<uint32>(Address));
        FBlueprintNode* const NewNode = PushNewNode(NodeName_Sequence, InOpcode);
        NewNode->WiresToProcess.Push(FProcessWireData{CurrentFunction, SanitiseAddress(Address)});
        break;
    }
    case EX_PopExecutionFlow:
    {
        return FinishFlow(InOpcode);
    }
    case EX_PopExecutionFlowIfNot:
    {
        PushNewNode(NodeName_PopExecutionFlowIfNot, InOpcode);
        DecodeOpcode(EDirection::Input);
        // The false arm pops the execution flow back past the enclosing EX_PushExecutionFlow. The walk
        // keeps going linearly here: the renderer has no construct for the pop target yet, and adding it
        // as an ordinary output wire placed the target inside the block instead of after it.
        break;
    }

    case EX_Breakpoint:
    case EX_Tracepoint:
    case EX_WireTracepoint:
        break;

    case EX_InstrumentationEvent:
    {
        // Bytecode: the event byte, then the event's FScriptName when the event is InlineEvent. The
        // name is not an operand, but both the engine's serialization and its exec handler step over
        // it, so leaving it unread shifts every following opcode by 12 bytes.
        uint8 EventType = 0;
        ScriptReader << EventType;

        if (EventType == EScriptInstrumentation::InlineEvent)
        {
            FScriptName EventName;
            ScriptReader << EventName;
            OpcodeLogger.LogValue(BI_ScriptNameToString(EventName));
        }
        break;
    }

    case EX_SwitchValue:
    {
        uint16 NumberOfCases = 0;
        CodeSkipSizeType OffsetToEnd = 0;

        ScriptReader << NumberOfCases << OffsetToEnd;

        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const NewNode = PushNewNode(NodeName_Switch, InOpcode);
        DecodeOpcode(EDirection::Input);

        for (uint16 CaseIndex = 0; CaseIndex < NumberOfCases; ++CaseIndex)
        {
            DecodeOpcode(EDirection::Input);
            CodeSkipSizeType OffsetToNextCase = 0;
            ScriptReader << OffsetToNextCase;
            DecodeOpcode(EDirection::Input);
        }
        DecodeOpcode(EDirection::Input);
        if (InDirection == EDirection::Input && Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, NewNode);
            Stack.Pop();
        }
        break;
    }
    // Bytecode: the array expression, then the index. The visualizer has no case for this opcode, so it only
    // matters as an assignment target: ReturnNode is repointed at the array's INNER element, i.e. the
    // element type rather than the array type.
    case EX_ArrayGetByRef:
    {
        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 FirstInputIndex = CurrentNode->InputNodes.Num();
        DecodeOpcode(EDirection::Input);
        DecodeOpcode(EDirection::Input);

        if (InDirection == EDirection::Output && CurrentNode->InputNodes.IsValidIndex(FirstInputIndex))
        {
            const FContextProperty* ArrayContext = CurrentNode->InputNodes[FirstInputIndex].TryGet<FContextProperty>();
            if (ArrayContext && ArrayContext->Property)
            {
                if (const FArrayProperty* ArrayProperty = CastField<FArrayProperty>(ArrayContext->Property))
                {
                    CurrentNode->ReturnNode = FContextProperty{ArrayContext->Context, ArrayProperty->Inner};
                }
            }
        }
        break;
    }
#if BI_ENGINE_AT_LEAST_5_4
    // AutoRTFM transaction markers. Skipped through so the contents are still walked, but no node is built
    // for the markers themselves.
    case EX_AutoRtfmTransact:
    {
        int32 Value = 0;
        CodeSkipSizeType SkipCount = 0;
        ScriptReader << Value << SkipCount;
        DecodeOpcodeUntil(EX_AutoRtfmStopTransact, InDirection);
        break;
    }
    case EX_AutoRtfmStopTransact:
    {
        int32 Value = 0;
        EAutoRtfmStopTransactMode Mode = static_cast<EAutoRtfmStopTransactMode>(0);
        ScriptReader << Value << reinterpret_cast<uint8&>(Mode);
        break;
    }
    case EX_AutoRtfmAbortIfNot:
    {
        DecodeOpcode(EDirection::Input);
        break;
    }
#endif

    // Structure terminators. They carry no payload; their only job is to stop the matching
    // DecodeOpcodeUntil loop in the opcode that opened the construct.
    case EX_EndFunctionParms:
    case EX_EndArrayConst:
    case EX_EndArray:
    case EX_EndMapConst:
    case EX_EndMap:
    case EX_EndSet:
    case EX_EndSetConst:
    case EX_EndOfScript:
    case EX_EndStructConst:
        break;

    case EX_IntZero:
    case EX_IntOne:
    case EX_True:
    case EX_False:
    case EX_Self:
    case EX_NoObject:
    case EX_NoInterface:
    case EX_EndParmValue:
    case EX_DeprecatedOp4A:
    case EX_Nothing:
        if (InDirection == EDirection::Input)
        {
            Stack.Top()->InputNodes.Emplace_GetRef().Emplace<EExprToken>(InOpcode);
        }
        break;

    // Unhandled opcode. New EExprToken values have to be given an explicit case here, otherwise the operand
    // sizes are unknown and the rest of the stream is decoded at the wrong offset. Fail soft so a single
    // unknown opcode cannot hard-break the whole walk.
    default:
    {
        OpcodeLogger.CountUnsupported();
        UE_LOG(LogBlueprintInspector, Error, TEXT("Unhandled opcode 0x%.2X at script offset %lld"), static_cast<uint8>(InOpcode), CurrentOpcodeScriptIndex);
        ScriptReader.SetError();
        break;
    }
    }

    return InOpcode;
}

EExprToken FBlueprintScriptDisassembler::FinishFlow(const EExprToken InOpcode)
{
    while (Stack.Num() && !Stack.Top()->WiresToProcess.Num())
    {
        Stack.Pop(BI_POP_NO_SHRINK);
    }

    if (!Stack.Num())
    {
        return EX_EndOfScript;
    }

    if (Stack.Num() && Stack.Top()->WiresToProcess.Num())
    {
        // Whether the current path has already decoded this (function, address). Bytecode offsets are
        // function-local, so the function is part of the comparison: a jump target in another function
        // that shares the offset is not a loop. (A target decoded only through a sibling branch is
        // deliberately decoded again; the duplicated node is how the renderer sees a convergent edge.)
        const auto IsOnCurrentPath = [this](const FProcessWireData& Wire) {
            TArray<const FBlueprintNode*, TInlineAllocator<InlineBlueprintNodeQueueCapacity>> BlueprintNodeQueue{Stack.Top()};
            while (BlueprintNodeQueue.Num() != 0)
            {
                const FBlueprintNode* const BlueprintNode = BlueprintNodeQueue.Pop(BI_POP_NO_SHRINK);
                if (BlueprintNode->ScriptIndex == Wire.Address && BlueprintNode->FunctionOwner == Wire.Function)
                {
                    return true;
                }
                for (const FBlueprintNode* const InputWire : BlueprintNode->InputWires)
                {
                    BlueprintNodeQueue.Push(InputWire);
                }
            }
            return false;
        };

        FProcessWireData NextFlowStart = Stack.Top()->WiresToProcess.Pop();

        // Drain queued arms until one lands on an address this path has not taken. Applying the check to
        // every queued wire matters after a back edge: the second arm of the same branch can also be
        // visited, and dispatching it blindly re-decoded the same subgraph.
        for (;;)
        {
            if (!IsOnCurrentPath(NextFlowStart))
            {
                // SanitiseAddress() passes wild and out-of-range targets through, so the queued wire can
                // name an address the reader cannot be positioned at. The bytes at the current offset
                // then belong to a different transfer, so stop instead of mis-decoding them.
                if (!SeekToOffset(NextFlowStart.Function, NextFlowStart.Address))
                {
                    ScriptReader.SetError();
                    return EX_EndOfScript;
                }

                // A jump the walk has not taken before: the node it creates at the target is where a
                // `break` to the loop exit is rendered, so the dispatch pair travels with the arrival.
                PendingArrivalAddress = NextFlowStart.Address;
                PendingArrivalJumpOpcode = NextFlowStart.JumpOpcode;
                PendingArrivalLanding = NextFlowStart.Landing;
                break;
            }

            // The edge closes a loop. Keep it on the node that queued it so the structurer can render the
            // loop instead of silently dropping the transfer. The output-wire count is the arm the jump
            // would have occupied, which lets the renderer tell a branch arm that continues from a
            // linear latch.
            if (FBlueprintNode* const JumpFrom = Stack.Top())
            {
                JumpFrom->BackEdgeTarget = NextFlowStart.Address;
                JumpFrom->BackEdgeWire = JumpFrom->OutputWires.Num();
                JumpFrom->BackEdgeJumpOpcode = NextFlowStart.JumpOpcode;
                JumpFrom->BackEdgeLanding = NextFlowStart.Landing;
            }

            while (Stack.Num() && !Stack.Top()->WiresToProcess.Num())
            {
                Stack.Pop(BI_POP_NO_SHRINK);
            }

            if (!Stack.Num())
            {
                return EX_EndOfScript;
            }

            NextFlowStart = Stack.Top()->WiresToProcess.Pop(BI_POP_NO_SHRINK);
        }
    }

    return InOpcode;
}

int64 FBlueprintScriptDisassembler::SanitiseAddress(const int64 InAddress)
{
    // Only probe an address that lies inside this function's script. A wild target (or a negative
    // INDEX_NONE left by a previous step) would make Seek raise the archive's error flag, and every
    // later read treats that flag as fatal, aborting the whole decode instead of just this probe.
    const int64 ScriptLength = static_cast<int64>(ScriptReader.Length());
    if (InAddress < 0 || InAddress > ScriptLength)
    {
        return InAddress;
    }

    // In editor builds, the wires could point to debug opcodes which we don't capture, so if
    // we're targeting one of those, bypass to an opcode we do.
    const int64 OriginalAddress = ScriptReader.Tell();
    ScriptReader.Seek(InAddress);

    int64 NewAddress = -1;
    while (!ScriptReader.AtEnd() && NewAddress == -1)
    {
        if (ScriptLength - ScriptReader.Tell() < 1)
        {
            break;
        }
        uint8 OpcodeByte = 0;
        if (!ScriptReader.ReadByte(OpcodeByte))
        {
            break;
        }

        const EExprToken Opcode = static_cast<EExprToken>(OpcodeByte);
        if (Opcode == EX_Max)
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("SanitiseAddress encountered EX_Max at offset %lld"), ScriptReader.Tell() - 1);
            // Stop the probe: leaving only the switch would keep scanning the bytes after EX_Max.
            NewAddress = InAddress;
            break;
        }
        switch (Opcode)
        {
        case EX_Breakpoint:
        case EX_Tracepoint:
        case EX_WireTracepoint:
            break;
        case EX_InstrumentationEvent:
        {
            // InlineEvent stores the event's FScriptName after the event byte, and the engine's
            // serialization skips those bytes too. Stop the probe if the operand is truncated, instead
            // of letting Skip raise the archive's fatal error flag.
            uint8 EventType = 0;
            if (ScriptReader.Length() - ScriptReader.Tell() < 1 || !ScriptReader.ReadByte(EventType))
            {
                break;
            }
            if (EventType == EScriptInstrumentation::InlineEvent)
            {
                if (ScriptReader.Length() - ScriptReader.Tell() < static_cast<int64>(sizeof(FScriptName)))
                {
                    NewAddress = ScriptReader.Tell() - 2;
                    break;
                }
                ScriptReader.Skip(sizeof(FScriptName));
            }
            break;
        }
        default:
            NewAddress = ScriptReader.Tell() - 1;
            break;
        }
    }

    ScriptReader.Seek(OriginalAddress);
    return NewAddress != -1 ? NewAddress : InAddress;
}

// Two distinct edge lists are maintained, and the difference matters:
//   OutputWires: execution order. The new node is appended here and the visualizer walks this to emit
//                 statements, so a node only appears in the output if it was reached through here.
//   InputWires: reverse (parent) links, purely for FinishFlow's already-visited check. They stop the
//                 walker from re-parsing a path it has already taken and looping forever.
FBlueprintNode* FBlueprintScriptDisassembler::PushNewNode(const FName& InName, const EExprToken InOpcode)
{
    // Consecutive EX_PushExecutionFlow opcodes fold into one node: the compiler emits one per sequence pin.
    FBlueprintNode* const CurrentNode = Stack.Top();
    if (InOpcode != EX_PushExecutionFlow || CurrentNode->Opcode != EX_PushExecutionFlow)
    {
        const int32 NodeIndex = static_cast<int32>(NodeIterator - Nodes.GetData());
        if (NodeIndex < Nodes.Num())
        {
            FBlueprintNode* const NewNode = NodeIterator++;
            *NewNode = FBlueprintNode(CurrentFunction, InName, InOpcode);
            NewNode->ScriptIndex = CurrentOpcodeScriptIndex;

            // The edge that brought the walk here is what a `break` on this line has to arm.
            if (NewNode->ScriptIndex == PendingArrivalAddress)
            {
                NewNode->ArrivalJumpOpcode = PendingArrivalJumpOpcode;
                NewNode->ArrivalLanding = PendingArrivalLanding;
                PendingArrivalAddress = INDEX_NONE;
                PendingArrivalJumpOpcode = INDEX_NONE;
                PendingArrivalLanding = INDEX_NONE;
            }
            NewNode->InputWires.Reserve(1);
            NewNode->OutputWires.Reserve(2);
            NewNode->InputNodes.Reserve(InitialInputNodeReserve);

            // A parked receiver is the context's object expression, and it belongs to the first node the member
            // decode creates: the member itself, which is the node that displays "Object->Member". Anything
            // that reaches here without a receiver parked finds the slot empty.
            if (PendingReceivers.Num() > 0)
            {
                NewNode->Receiver = MoveTemp(PendingReceivers.Last());
                PendingReceivers.Last().Reset();
            }
            Stack.Top()->OutputWires.Emplace_GetRef().Emplace<FBlueprintNode*>(NewNode);
            Stack.Push(NewNode);
            NewNode->InputWires.Push(CurrentNode);
            return NewNode;
        }

        // Pool exhausted: keep going with heap nodes instead of folding everything into one scratch
        // node. Each overflow node keeps its own address and opcode, so wires created earlier stay
        // valid and the LiftToOperand/pop pattern in the callers cannot see a self-reference.
        if (!bNodePoolExhaustedLogged)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Node pool exhausted (%d nodes); allocating further nodes from the overflow pool"), Nodes.Num());
            bNodePoolExhaustedLogged = true;
        }
        OverflowNodes.Emplace(MakeUnique<FBlueprintNode>(CurrentFunction, InName, InOpcode));
        FBlueprintNode* const NewNode = OverflowNodes.Last().Get();
        NewNode->InputWires.Reserve(1);
        NewNode->OutputWires.Reserve(2);
        NewNode->InputNodes.Reserve(InitialInputNodeReserve);
        NewNode->ScriptIndex = CurrentOpcodeScriptIndex;
        if (NewNode->ScriptIndex == PendingArrivalAddress)
        {
            NewNode->ArrivalJumpOpcode = PendingArrivalJumpOpcode;
            NewNode->ArrivalLanding = PendingArrivalLanding;
            PendingArrivalAddress = INDEX_NONE;
            PendingArrivalJumpOpcode = INDEX_NONE;
            PendingArrivalLanding = INDEX_NONE;
        }
        if (PendingReceivers.Num() > 0)
        {
            NewNode->Receiver = MoveTemp(PendingReceivers.Last());
            PendingReceivers.Last().Reset();
        }
        Stack.Top()->OutputWires.Emplace_GetRef().Emplace<FBlueprintNode*>(NewNode);
        Stack.Push(NewNode);
        NewNode->InputWires.Push(CurrentNode);
        return NewNode;
    }
    return CurrentNode;
}

FBlueprintNode* FBlueprintScriptDisassembler::DecodeFunctionData(const EExprToken InOpcode, const EDirection InDirection, const UFunction* const InFunction)
{
    if (!IsValid(InFunction))
    {
        // The call could not be resolved, but its parameter list is still in the stream. Consume it
        // against a scratch node so the arguments do not decode as statements of the enclosing function.
        FBlueprintNode* const CurrentNode = Stack.Top();
        const int32 StackDepth = Stack.Num();
        FBlueprintNode* const ScratchNode = PushNewNode(NodeName_UnresolvedCall, InOpcode);

        for (EExprToken Opcode = EX_Max; Opcode != EX_EndFunctionParms && Opcode != EX_EndOfScript && !ScriptReader.IsError();)
        {
            Opcode = DecodeOpcode(EDirection::Input);
        }

        if (InDirection == EDirection::Input && Stack.Num() > StackDepth)
        {
            LiftToOperand(*CurrentNode, ScratchNode);
            Stack.Pop();
        }

        return nullptr;
    }

    OpcodeLogger.LogValue(InFunction->GetName());
    OpcodeLogger.Indent();

    FBlueprintNode* const CurrentNode = Stack.Top();
    const int32 StackDepth = Stack.Num();
    FBlueprintNode* const NewNode = PushNewNode(InFunction->GetFName(), InOpcode);
    // Metadata is compiled out of a game target (WITH_METADATA is 0 without editor data) and
    // HasMetaData does not exist there, so the Shipping build must not call it. Without metadata a
    // shipped build reports no world context; the editor build keeps the flag.
#if WITH_METADATA
    NewNode->bCalleeHasWorldContext = InFunction->HasMetaData(TEXT("WorldContext"));
#endif

    for (EExprToken Opcode = EX_Max; Opcode != EX_EndFunctionParms && Opcode != EX_EndOfScript && !ScriptReader.IsError();)
    {
        Opcode = DecodeOpcode(EDirection::Input);
    }

    if (InDirection == EDirection::Input && Stack.Num() > StackDepth)
    {
        LiftToOperand(*CurrentNode, NewNode);
        Stack.Pop();
    }

    OpcodeLogger.Unindent();
    return NewNode;
}