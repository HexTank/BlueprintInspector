#include "BlueprintOpcodeLogger.h"
#include "BlueprintInspector.h"
#include "BlueprintInspectorPort.h"
#include "HAL/IConsoleManager.h"
#include "UObject/Field.h"
#include "UObject/Object.h"

namespace
{
TAutoConsoleVariable<int32> CVarLogOpcodes(TEXT("bp.LogOpcodes"), 0, TEXT("Log every decoded Blueprint opcode and its operands."), ECVF_Default);

const TCHAR* const ValuePrefix = TEXT("     ");

TAtomic<bool> bUnknownOpcodeLogged{false};
} // namespace

bool FBlueprintOpcodeLogger::IsEnabled()
{
    return CVarLogOpcodes.GetValueOnAnyThread() != 0;
}

FCriticalSection FBlueprintOpcodeLogger::SinkLock;
FBlueprintOpcodeLogger::FOpcodeSink FBlueprintOpcodeLogger::Sink;
TAtomic<bool> FBlueprintOpcodeLogger::bSinkSet{false};
FCriticalSection FBlueprintOpcodeLogger::SummarySinkLock;
FBlueprintOpcodeLogger::FSummarySink FBlueprintOpcodeLogger::SummarySink;

void FBlueprintOpcodeLogger::BeginRun()
{
    // An error path can leave a scope open; the next function must start at the base indent. Both the
    // counters and the indent are reset under the same lock the logging paths use. The cached indent
    // string has to be rebuilt too: IndentLevel alone would leave the previous run's padding in place
    // until the first Indent().
    FScopeLock Lock(&StatsLock);
    Stats = FBlueprintOpcodeStats{};
    IndentLevel = 0;
    UpdateIndentString();
}

void FBlueprintOpcodeLogger::SetSummarySink(FSummarySink InSink)
{
    FSummarySink Previous;
    {
        FScopeLock Lock(&SummarySinkLock);
        Previous = MoveTemp(SummarySink);
        SummarySink = MoveTemp(InSink);
    }
}

void FBlueprintOpcodeLogger::ClearSummarySink()
{
    FSummarySink Previous;
    {
        FScopeLock Lock(&SummarySinkLock);
        Previous = MoveTemp(SummarySink);
    }
}

void FBlueprintOpcodeLogger::LogSummary(const FString& FunctionName) const
{
    FBlueprintOpcodeStats Snapshot;
    {
        FScopeLock Lock(&StatsLock);
        Snapshot = Stats;
    }

    FSummarySink LocalSink;
    {
        FScopeLock Lock(&SummarySinkLock);
        LocalSink = SummarySink;
    }
    if (LocalSink)
    {
        LocalSink(FunctionName, Snapshot);
    }

    if (IsEnabled())
    {
        UE_LOG(LogBlueprintInspector, Log, TEXT("Opcode summary: function=%s opcodes=%d unsupported=%d"), *FunctionName, Snapshot.Opcodes, Snapshot.Unsupported);
    }
}

void FBlueprintOpcodeLogger::CountUnsupported()
{
    FScopeLock Lock(&StatsLock);
    ++Stats.Unsupported;
}

void FBlueprintOpcodeLogger::SetOpcodeSink(FOpcodeSink InSink)
{
    FOpcodeSink Previous;
    {
        FScopeLock Lock(&SinkLock);
        Previous = MoveTemp(Sink);
        Sink = MoveTemp(InSink);
        bSinkSet.Store(Sink != nullptr);
    }
}

void FBlueprintOpcodeLogger::ClearOpcodeSink()
{
    FOpcodeSink Previous;
    {
        FScopeLock Lock(&SinkLock);
        Previous = MoveTemp(Sink);
        bSinkSet.Store(false);
    }
}

FString FBlueprintOpcodeLogger::OpcodeToString(const EExprToken InOpcode)
{
    return GetOpcodeName(InOpcode);
}

void FBlueprintOpcodeLogger::LogOpcode(const EExprToken InOpcode, const int64 InScriptIndex)
{
    {
        FScopeLock Lock(&StatsLock);
        ++Stats.Opcodes;
    }

    FOpcodeSink LocalSink;
    if (bSinkSet.Load())
    {
        FScopeLock Lock(&SinkLock);
        LocalSink = Sink;
    }
    if (LocalSink)
    {
        LocalSink(InOpcode, InScriptIndex);
    }

    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %5lld %s 0x%.2X // %s"), InScriptIndex, *GetIndentString(), static_cast<unsigned int>(static_cast<uint8>(InOpcode)), GetOpcodeName(InOpcode));
}

void FBlueprintOpcodeLogger::LogValue(const TCHAR* const InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %s"), ValuePrefix, *GetIndentString(), InValue ? InValue : TEXT("nullptr"));
}

void FBlueprintOpcodeLogger::LogValue(const FString& InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %s"), ValuePrefix, *GetIndentString(), *InValue);
}

void FBlueprintOpcodeLogger::LogValue(const FScriptName& InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    const FString Value = BI_ScriptNameToString(InValue);
    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %s"), ValuePrefix, *GetIndentString(), *Value);
}

void FBlueprintOpcodeLogger::LogValue(const int32 InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %d"), ValuePrefix, *GetIndentString(), InValue);
}

void FBlueprintOpcodeLogger::LogValue(const int64 InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %lld"), ValuePrefix, *GetIndentString(), InValue);
}

void FBlueprintOpcodeLogger::LogValue(const uint32 InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %u"), ValuePrefix, *GetIndentString(), InValue);
}

void FBlueprintOpcodeLogger::LogValue(const uint64 InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %llu"), ValuePrefix, *GetIndentString(), InValue);
}

void FBlueprintOpcodeLogger::LogValue(const double InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    // %.17g is DBL_DECIMAL_DIG: %.9g only round-trips a 32-bit float, so a double differing in the low
    // mantissa bits would print as the same constant.
    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %.17g"), ValuePrefix, *GetIndentString(), InValue);
}

void FBlueprintOpcodeLogger::LogValue(const bool InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %s"), ValuePrefix, *GetIndentString(), InValue ? TEXT("true") : TEXT("false"));
}

void FBlueprintOpcodeLogger::LogValue(const std::nullptr_t)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s nullptr"), ValuePrefix, *GetIndentString());
}

void FBlueprintOpcodeLogger::LogValue(const UObject* const InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %s"), ValuePrefix, *GetIndentString(), IsValid(InValue) ? *InValue->GetName() : TEXT("<invalid>"));
}

void FBlueprintOpcodeLogger::LogValue(const FField* const InValue)
{
    if (!IsEnabled())
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Expression: %s %s %s"), ValuePrefix, *GetIndentString(), InValue != nullptr ? *InValue->GetName() : TEXT("<invalid>"));
}

const TCHAR* FBlueprintOpcodeLogger::GetOpcodeName(const EExprToken InOpcode)
{
    switch (InOpcode)
    {
    case EX_LocalVariable:
        return TEXT("EX_LocalVariable");
    case EX_InstanceVariable:
        return TEXT("EX_InstanceVariable");
    case EX_DefaultVariable:
        return TEXT("EX_DefaultVariable");
    case EX_Return:
        return TEXT("EX_Return");
    case EX_Jump:
        return TEXT("EX_Jump");
    case EX_JumpIfNot:
        return TEXT("EX_JumpIfNot");
    case EX_Assert:
        return TEXT("EX_Assert");
    case EX_Nothing:
        return TEXT("EX_Nothing");
    case EX_Let:
        return TEXT("EX_Let");
    case EX_ClassContext:
        return TEXT("EX_ClassContext");
    case EX_MetaCast:
        return TEXT("EX_MetaCast");
    case EX_LetBool:
        return TEXT("EX_LetBool");
    case EX_EndParmValue:
        return TEXT("EX_EndParmValue");
    case EX_EndFunctionParms:
        return TEXT("EX_EndFunctionParms");
    case EX_Self:
        return TEXT("EX_Self");
    case EX_Skip:
        return TEXT("EX_Skip");
    case EX_Context:
        return TEXT("EX_Context");
    case EX_Context_FailSilent:
        return TEXT("EX_Context_FailSilent");
    case EX_VirtualFunction:
        return TEXT("EX_VirtualFunction");
    case EX_FinalFunction:
        return TEXT("EX_FinalFunction");
    case EX_IntConst:
        return TEXT("EX_IntConst");
    case EX_FloatConst:
        return TEXT("EX_FloatConst");
    case EX_StringConst:
        return TEXT("EX_StringConst");
    case EX_ObjectConst:
        return TEXT("EX_ObjectConst");
    case EX_NameConst:
        return TEXT("EX_NameConst");
    case EX_RotationConst:
        return TEXT("EX_RotationConst");
    case EX_VectorConst:
        return TEXT("EX_VectorConst");
    case EX_ByteConst:
        return TEXT("EX_ByteConst");
    case EX_IntZero:
        return TEXT("EX_IntZero");
    case EX_IntOne:
        return TEXT("EX_IntOne");
    case EX_True:
        return TEXT("EX_True");
    case EX_False:
        return TEXT("EX_False");
    case EX_TextConst:
        return TEXT("EX_TextConst");
    case EX_NoObject:
        return TEXT("EX_NoObject");
    case EX_TransformConst:
        return TEXT("EX_TransformConst");
    case EX_IntConstByte:
        return TEXT("EX_IntConstByte");
    case EX_NoInterface:
        return TEXT("EX_NoInterface");
    case EX_DynamicCast:
        return TEXT("EX_DynamicCast");
    case EX_StructConst:
        return TEXT("EX_StructConst");
    case EX_EndStructConst:
        return TEXT("EX_EndStructConst");
    case EX_SetArray:
        return TEXT("EX_SetArray");
    case EX_EndArray:
        return TEXT("EX_EndArray");
    case EX_UnicodeStringConst:
        return TEXT("EX_UnicodeStringConst");
    case EX_Int64Const:
        return TEXT("EX_Int64Const");
    case EX_UInt64Const:
        return TEXT("EX_UInt64Const");
    case EX_SetSet:
        return TEXT("EX_SetSet");
    case EX_EndSet:
        return TEXT("EX_EndSet");
    case EX_SetMap:
        return TEXT("EX_SetMap");
    case EX_EndMap:
        return TEXT("EX_EndMap");
    case EX_SetConst:
        return TEXT("EX_SetConst");
    case EX_EndSetConst:
        return TEXT("EX_EndSetConst");
    case EX_MapConst:
        return TEXT("EX_MapConst");
    case EX_EndMapConst:
        return TEXT("EX_EndMapConst");
    case EX_StructMemberContext:
        return TEXT("EX_StructMemberContext");
    case EX_LetMulticastDelegate:
        return TEXT("EX_LetMulticastDelegate");
    case EX_LetDelegate:
        return TEXT("EX_LetDelegate");
    case EX_LocalVirtualFunction:
        return TEXT("EX_LocalVirtualFunction");
    case EX_LocalFinalFunction:
        return TEXT("EX_LocalFinalFunction");
    case EX_LocalOutVariable:
        return TEXT("EX_LocalOutVariable");
    case EX_DeprecatedOp4A:
        return TEXT("EX_DeprecatedOp4A");
    case EX_InstanceDelegate:
        return TEXT("EX_InstanceDelegate");
    case EX_PushExecutionFlow:
        return TEXT("EX_PushExecutionFlow");
    case EX_PopExecutionFlow:
        return TEXT("EX_PopExecutionFlow");
    case EX_ComputedJump:
        return TEXT("EX_ComputedJump");
    case EX_PopExecutionFlowIfNot:
        return TEXT("EX_PopExecutionFlowIfNot");
    case EX_Breakpoint:
        return TEXT("EX_Breakpoint");
    case EX_InterfaceContext:
        return TEXT("EX_InterfaceContext");
    case EX_ObjToInterfaceCast:
        return TEXT("EX_ObjToInterfaceCast");
    case EX_EndOfScript:
        return TEXT("EX_EndOfScript");
    case EX_CrossInterfaceCast:
        return TEXT("EX_CrossInterfaceCast");
    case EX_InterfaceToObjCast:
        return TEXT("EX_InterfaceToObjCast");
    case EX_WireTracepoint:
        return TEXT("EX_WireTracepoint");
    case EX_SkipOffsetConst:
        return TEXT("EX_SkipOffsetConst");
    case EX_AddMulticastDelegate:
        return TEXT("EX_AddMulticastDelegate");
    case EX_ClearMulticastDelegate:
        return TEXT("EX_ClearMulticastDelegate");
    case EX_Tracepoint:
        return TEXT("EX_Tracepoint");
    case EX_LetObj:
        return TEXT("EX_LetObj");
    case EX_LetWeakObjPtr:
        return TEXT("EX_LetWeakObjPtr");
    case EX_BindDelegate:
        return TEXT("EX_BindDelegate");
    case EX_RemoveMulticastDelegate:
        return TEXT("EX_RemoveMulticastDelegate");
    case EX_CallMulticastDelegate:
        return TEXT("EX_CallMulticastDelegate");
    case EX_LetValueOnPersistentFrame:
        return TEXT("EX_LetValueOnPersistentFrame");
    case EX_ArrayConst:
        return TEXT("EX_ArrayConst");
    case EX_EndArrayConst:
        return TEXT("EX_EndArrayConst");
    case EX_SoftObjectConst:
        return TEXT("EX_SoftObjectConst");
    case EX_CallMath:
        return TEXT("EX_CallMath");
    case EX_SwitchValue:
        return TEXT("EX_SwitchValue");
    case EX_InstrumentationEvent:
        return TEXT("EX_InstrumentationEvent");
    case EX_ArrayGetByRef:
        return TEXT("EX_ArrayGetByRef");
    case EX_FieldPathConst:
        return TEXT("EX_FieldPathConst");

#if BI_ENGINE_AT_LEAST_5_3
    case EX_NothingInt32:
        return TEXT("EX_NothingInt32");
    case EX_BitFieldConst:
        return TEXT("EX_BitFieldConst");
#endif

#if BI_ENGINE_AT_LEAST_5
    case EX_PropertyConst:
        return TEXT("EX_PropertyConst");
    case EX_DoubleConst:
        return TEXT("EX_DoubleConst");
    case EX_Cast:
        return TEXT("EX_Cast");
    case EX_Vector3fConst:
        return TEXT("EX_Vector3fConst");
#else
    case EX_PrimitiveCast:
        return TEXT("EX_PrimitiveCast");
#endif
#if BI_ENGINE_AT_LEAST_5_1
    case EX_ClassSparseDataVariable:
        return TEXT("EX_ClassSparseDataVariable");
#endif
#if BI_ENGINE_AT_LEAST_5_4
    case EX_AutoRtfmTransact:
        return TEXT("EX_AutoRtfmTransact");
    case EX_AutoRtfmStopTransact:
        return TEXT("EX_AutoRtfmStopTransact");
    case EX_AutoRtfmAbortIfNot:
        return TEXT("EX_AutoRtfmAbortIfNot");
#endif
    case EX_Max:
        return TEXT("EX_Max");
    default:
        if (!bUnknownOpcodeLogged.Load())
        {
            bUnknownOpcodeLogged.Store(true);
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Opcode logger has no name for opcode %u; the name table is out of sync with this engine version"), static_cast<uint32>(static_cast<uint8>(InOpcode)));
        }
        return TEXT("<unknown>");
    }
}
