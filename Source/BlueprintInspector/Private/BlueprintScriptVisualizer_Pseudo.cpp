#include "BlueprintScriptVisualizer_Pseudo.h"
#include "BlueprintInspector.h"
#include "Misc/ScopeExit.h"

#include "GameFramework/Actor.h"
#include "UObject/FieldIterator.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"

namespace
{
// Inline buffer sizes and the first-guess source-map reserve; none of these is a limit on the output.
constexpr int32 RenderedTextBuilderCapacity = 4096;
constexpr int32 MinSourceMapReserve = 64;
constexpr int32 ScriptBytesPerSourceMapLine = 8;
constexpr int32 LiteralBuilderCapacity = 512;
constexpr int32 SwitchBuilderCapacity = 1024;

bool IsIdentifierName(const FString& Text)
{
    if (Text.IsEmpty() || (!FChar::IsAlpha(Text[0]) && Text[0] != TEXT('_')))
    {
        return false;
    }
    for (const TCHAR Character : Text)
    {
        if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
        {
            return false;
        }
    }
    return true;
}
} // namespace

enum FBlueprintScriptVisualizer_Pseudo::ELineAppendMode : uint8
{
    StartScope,
    EndScope,
    EndScopeNoReturn,
    AddLine,
    StartLine,
    Append,
    AppendLine,
};

// The caches return copies: a TMap element can move on a later insert, so a TCHAR* into a cached FString
// would dangle. Presence is tested with Find, so a legitimately empty result is cached too.
FString FBlueprintScriptVisualizer_Pseudo::NormaliseString(const FString& Value)
{
    return SanitiseIdentifier(Value);
}

// A Blueprint can name a variable "Hit Actor" or "Dot Product - Forward Hit". Rendered code has to stay
// valid, so anything outside [A-Za-z0-9_] becomes an underscore, and runs of underscores collapse.
FString FBlueprintScriptVisualizer_Pseudo::SanitiseIdentifier(const FString& Value)
{
    // A qualified call renders as `Class::Function`; the separator is syntax, not part of the name.
    TArray<FString> Segments;
    Value.ParseIntoArray(Segments, TEXT("::"), false);
    if (Segments.Num() > 1)
    {
        TArray<FString> Sanitised;
        Sanitised.Reserve(Segments.Num());
        for (const FString& Segment : Segments)
        {
            Sanitised.Add(SanitiseIdentifier(Segment));
        }
        return FString::Join(Sanitised, TEXT("::"));
    }

    FString Result;
    Result.Reserve(Value.Len());
    for (int32 Index = 0; Index < Value.Len(); ++Index)
    {
        const TCHAR Character = Value[Index];
        const TCHAR Substituted = (FChar::IsAlnum(Character) || Character == TEXT('_')) ? Character : TEXT('_');
        if (Substituted == TEXT('_') && Result.Len() > 0 && Result[Result.Len() - 1] == TEXT('_'))
        {
            continue;
        }
        Result.AppendChar(Substituted);
    }
    if (Result.IsEmpty())
    {
        return TEXT("_");
    }
    if (FChar::IsDigit(Result[0]))
    {
        Result.InsertAt(0, TEXT('_'));
    }
    return Result;
}

FString FBlueprintScriptVisualizer_Pseudo::GetString(const FField* const Value) const
{
    if (Value == nullptr)
    {
        return FString();
    }

    if (const FString* const Cached = FieldCache.Find(Value))
    {
        return *Cached;
    }

    const FString Result = NormaliseString(Value->GetName());
    FieldCache.Add(Value, Result);
    return Result;
}

FString FBlueprintScriptVisualizer_Pseudo::GetString(const FProperty* const Value) const
{
    if (Value == nullptr)
    {
        return FString();
    }

    if (const FString* const Cached = PropertyCache.Find(Value))
    {
        return *Cached;
    }

    const FString Result = NormaliseString(Value->GetCPPType());
    PropertyCache.Add(Value, Result);
    return Result;
}

FString FBlueprintScriptVisualizer_Pseudo::GetString(const UFunction* const Value) const
{
    if (Value == nullptr)
    {
        return FString();
    }

    if (const FString* const Cached = FunctionCache.Find(Value))
    {
        return *Cached;
    }

    const FString Result = NormaliseString(Value->GetName());
    FunctionCache.Add(Value, Result);
    return Result;
}

// A CDO receiver is static in this context: its members are emitted fully qualified rather than prefixed, so
// it contributes no "Object->" even though the decoder keeps it as the receiver the member resolves against.
static bool IsStaticReceiver(const FBlueprintNode::FNodeVariant& InReceiver)
{
    const UObject* const* const Object = InReceiver.TryGet<const UObject*>();
    return Object != nullptr && *Object != nullptr && (*Object)->HasAnyFlags(RF_ClassDefaultObject);
}

const UObject* FBlueprintScriptVisualizer_Pseudo::GetObjectOperand(const FBlueprintNode::FNodeVariant& InVariant)
{
    const UObject* const* const Found = InVariant.TryGet<const UObject*>();
    return Found ? *Found : nullptr;
}

FString FBlueprintScriptVisualizer_Pseudo::GetCastTypeName(const UObject* const Value, const bool bInterface) const
{
    // Cast<> bytecode operands are UClass objects. Interface casts announce their target in the
    // opcode itself, and interface classes also carry CLASS_Interface, so either source is authoritative.
    if (const UClass* const Class = Cast<UClass>(Value))
    {
        const TCHAR* const Prefix = (bInterface || Class->HasAnyClassFlags(CLASS_Interface)) ? TEXT("I") : Class->IsChildOf(AActor::StaticClass()) ? TEXT("A")
                                                                                                                                                   : TEXT("U");

        return FString::Printf(TEXT("%s%s"), Prefix, *Class->GetName());
    }

    return Value ? *Value->GetName() : TEXT("");
}

void FBlueprintScriptVisualizer_Pseudo::Visualize(const TWeakObjectPtr<const UFunction>& InFunction, FBlueprintScriptDisassembler& InDisassembler, FString& OutScript, FBlueprintSourceMap* OutMap)
{
    if (OutMap)
    {
        OutMap->Lines.Reset();
    }

    // Clear before the early return: the caller may reuse one output string, and leaving the previous
    // function's text there reads as a successful render.
    OutScript.Reset();

    IndentLevel = 0;
    SourceMapBuilder.Reset();
    RenderedStatements.Reset();
    LoopStack.Reset();
    DoWhileLoops.Reset();
    CheckedDoWhile.Reset();
    WrappedLoops.Reset();
    // Everything below is keyed by node/name pointers from the previous disassembly, which the caller
    // may already have destroyed. A second Visualize() must not consult any of it.
    ComputedWhileLoops.Reset();
    CheckedComputedWhile.Reset();
    FoldedBranches.Reset();
    FieldCache.Reset();
    PropertyCache.Reset();
    FunctionCache.Reset();

    if (!InFunction.IsValid())
    {
        return;
    }

    FBlueprintNode::FWireVariant EntryPointNode;
    EntryPointNode.Emplace<FBlueprintNode*>(&InDisassembler.GetEntryPointNode());

    TStringBuilder<RenderedTextBuilderCapacity> StringBuilder;

    // The borrowed pointers are restored on every exit path (early return included), so a later visitor
    // can never write through a map or builder that a previous caller has destroyed.
    FBlueprintSourceMap* const PreviousSourceMap = ActiveSourceMap;
    const FStringBuilderBase* const PreviousTopLevelBuilder = ActiveTopLevelBuilder;
    ActiveSourceMap = OutMap;
    ActiveTopLevelBuilder = &StringBuilder;
    ON_SCOPE_EXIT
    {
        ActiveSourceMap = PreviousSourceMap;
        ActiveTopLevelBuilder = PreviousTopLevelBuilder;
    };

    SourceMapBuilder.Reserve(FMath::Max(MinSourceMapReserve, InFunction->Script.Num() / ScriptBytesPerSourceMapLine));

    bool bHasExistingParameter = false;

    FString NarrowString;
    NarrowString.Appendf(TEXT("%s("), *GetString(InFunction.Get()));
    AppendScript(StartLine, *NarrowString, StringBuilder);

    for (TFieldIterator<FField> FieldIterator(InFunction.Get()); FieldIterator; ++FieldIterator)
    {
        if (const FProperty* const Property = CastField<FProperty>(*FieldIterator))
        {
            if (Property->HasAllPropertyFlags(CPF_Parm) && !Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReturnParm))
            {
                StringBuilder.Appendf(TEXT("%s%s %s"), bHasExistingParameter ? TEXT(", ") : TEXT(""), *GetString(Property), *GetString(*FieldIterator));
                bHasExistingParameter = true;
            }
        }
    }

    AppendScript(AppendLine, TEXT(")"), StringBuilder);
    AppendScript(StartScope, TEXT(""), StringBuilder);
    RecurseNode(InFunction, EntryPointNode, StringBuilder);

    // A void function's bytecode always ends with an implicit "return;" (KismetCompilerVMBackend::EmitReturn
    // writes EX_Return + EX_Nothing), which carries no information, so drop it. A return that carries a value
    // is kept, as is one that sits earlier in the body.
    static const TCHAR ReturnSuffix[] = TEXT("return;\n");
    constexpr int32 ReturnSuffixLength = UE_ARRAY_COUNT(ReturnSuffix) - 1;
    const TCHAR* const IndentString = GetIndentString();
    const int32 IndentLength = FCString::Strlen(IndentString);
    const int32 SuffixLength = IndentLength + ReturnSuffixLength;
    const TCHAR* const Built = StringBuilder.ToString();
    const int32 BuiltLength = StringBuilder.Len();

    bool bEndsWithTrailingReturn = BuiltLength >= SuffixLength;
    for (int32 Index = 0; bEndsWithTrailingReturn && Index < ReturnSuffixLength; ++Index)
    {
        bEndsWithTrailingReturn = Built[BuiltLength - ReturnSuffixLength + Index] == ReturnSuffix[Index];
    }
    for (int32 Index = 0; bEndsWithTrailingReturn && Index < IndentLength; ++Index)
    {
        bEndsWithTrailingReturn = Built[BuiltLength - SuffixLength + Index] == IndentString[Index];
    }
    if (bEndsWithTrailingReturn)
    {
        // A return at a deeper indent still matches on its last IndentLength characters; only prune one
        // that starts its own line, or the remaining tabs are left behind.
        const int32 PrefixLength = BuiltLength - SuffixLength;
        bEndsWithTrailingReturn = PrefixLength == 0 || Built[PrefixLength - 1] == TEXT('\n');
    }
    if (bEndsWithTrailingReturn)
    {
        // The pruned line's own location described text that is no longer in the output, so drop it: the
        // closing brace records this same offset, and one record per rendered line is the map's contract.
        SourceMapBuilder.TrimFrom(BuiltLength - SuffixLength);
        StringBuilder.RemoveSuffix(SuffixLength);
    }

    AppendScript(EndScope, TEXT(""), StringBuilder);

    OutScript = StringBuilder.ToString();

    if (OutMap)
    {
        *OutMap = SourceMapBuilder.Build(OutScript);
    }
}

void FBlueprintScriptVisualizer_Pseudo::PushLocation(const FStringBuilderBase& InOutScriptBuilder, const FBlueprintLineInfo& InLineInfo) const
{
    // Only the top-level builder's offsets are indices into the rendered text. An operand visitor emits
    // into a private builder, whose lengths would add out-of-order records and corrupt the whole map.
    if (ActiveSourceMap && &InOutScriptBuilder == ActiveTopLevelBuilder)
    {
        SourceMapBuilder.AddLocation(InOutScriptBuilder.Len(), InLineInfo);
    }
}

// The VM exits its script loop on EX_Return without dispatching it, so an address on the return token
// can never hit a breakpoint. The compiler always writes the return value token immediately after
// EX_Return (EX_Nothing for a void return, otherwise the expression), and that token is dispatched.
// Point the line there so the stop happens before the value is evaluated.
static int64 RecordedStatementAddress(const FBlueprintNode& InNode)
{
    const UFunction* const Owner = InNode.FunctionOwner.Get();
    if (InNode.ScriptIndex < 0 || Owner == nullptr)
    {
        return INDEX_NONE;
    }

    const int64 Address = InNode.Opcode == EX_Return ? InNode.ScriptIndex + 1 : InNode.ScriptIndex;
    return Owner->Script.IsValidIndex(static_cast<int32>(Address)) ? Address : INDEX_NONE;
}

void FBlueprintScriptVisualizer_Pseudo::RecordStatement(const FBlueprintNode& InNode, const FStringBuilderBase& InOutScriptBuilder) const
{
    FBlueprintLineInfo Info;
    Info.OwnerFunction = InNode.FunctionOwner;
    Info.ScriptIndex = RecordedStatementAddress(InNode);
    Info.Kind = Info.ScriptIndex == INDEX_NONE ? EBlueprintLineKind::None : EBlueprintLineKind::Statement;

    PushLocation(InOutScriptBuilder, Info);
}

void FBlueprintScriptVisualizer_Pseudo::RecordStructural(const FStringBuilderBase& InOutScriptBuilder) const
{
    PushLocation(InOutScriptBuilder, FBlueprintLineInfo{});
}

bool FBlueprintScriptVisualizer_Pseudo::MarkStatementRendered(const FBlueprintNode& InNode)
{
    const UFunction* const Function = InNode.FunctionOwner.Get();
    if (Function == nullptr || InNode.ScriptIndex == INDEX_NONE || !Function->Script.IsValidIndex(static_cast<int32>(InNode.ScriptIndex)))
    {
        return true;
    }

    TSet<int64>& Addresses = RenderedStatements.FindOrAdd(Function);
    if (Addresses.Contains(InNode.ScriptIndex))
    {
        return false;
    }

    Addresses.Add(InNode.ScriptIndex);
    return true;
}

bool FBlueprintScriptVisualizer_Pseudo::HasRenderedStatement(const FBlueprintNode& InNode) const
{
    const UFunction* const Function = InNode.FunctionOwner.Get();
    if (Function == nullptr || InNode.ScriptIndex == INDEX_NONE || !Function->Script.IsValidIndex(static_cast<int32>(InNode.ScriptIndex)))
    {
        return false;
    }

    const TSet<int64>* const Addresses = RenderedStatements.Find(Function);
    return Addresses != nullptr && Addresses->Contains(InNode.ScriptIndex);
}

void FBlueprintScriptVisualizer_Pseudo::RenderBranchFlow(const FBlueprintNode& InBranch, FStringBuilderBase& OutScriptBuilder, const FFlowKey* const StopAt)
{
    // The walk pushes the false path first, so OutputWires[0] is the arm taken when the condition is false
    // and OutputWires[1] is the fallthrough. An arm that jumped back to a loop header creates no node, so a
    // branch can have one fewer wire than arms; BackEdgeWire says which arm is missing.
    const bool bArmTransfer = InBranch.BackEdgeTarget != INDEX_NONE && (InBranch.BackEdgeWire == 0 || InBranch.BackEdgeWire == 1);
    if (InBranch.OutputWires.Num() < 2 && !bArmTransfer)
    {
        return;
    }

    const auto WireForArm = [&InBranch](const int32 ArmIndex) -> const FBlueprintNode::FWireVariant* {
        if (InBranch.BackEdgeTarget != INDEX_NONE && InBranch.BackEdgeWire == 0)
        {
            // The false arm jumped back and created no node, so the true arm's node is at index 0.
            return ArmIndex == 1 && InBranch.OutputWires.IsValidIndex(0) ? &InBranch.OutputWires[0] : nullptr;
        }
        return InBranch.OutputWires.IsValidIndex(ArmIndex) ? &InBranch.OutputWires[ArmIndex] : nullptr;
    };

    BlueprintScriptFlow::FLoop WhileLoop;
    if (BlueprintScriptFlow::FindWhileLoop(InBranch, WhileLoop))
    {
        FLoopContext Context{WhileLoop.Header, WhileLoop.Exit, WhileLoop.Latch};
        LoopStack.Push(Context);
        AppendScript(StartScope, TEXT(""), OutScriptBuilder);
        if (const FBlueprintNode::FWireVariant* const BodyWire = WireForArm(1))
        {
            RecurseNode(InBranch.FunctionOwner, *BodyWire, OutScriptBuilder, StopAt);
        }
        AppendScript(EndScope, TEXT(""), OutScriptBuilder);
        LoopStack.Pop();

        if (const FBlueprintNode::FWireVariant* const ContinuationWire = WireForArm(0))
        {
            RecurseNode(InBranch.FunctionOwner, *ContinuationWire, OutScriptBuilder, StopAt);
        }
        return;
    }

    const FBlueprintNode* Merge = nullptr;
    FFlowKey MergeKey;
    const bool bHasMerge = BlueprintScriptFlow::FindMerge(InBranch, Merge, MergeKey);

    if (!bHasMerge || bArmTransfer)
    {
        // Unknown shape (irreducible flow, a sequence boundary) or an arm that jumps back to a loop header.
        // Follow each arm in turn; a transfer arm becomes break/continue instead of a walked statement.
        AppendScript(StartScope, TEXT(""), OutScriptBuilder);
        const int32 ArmCount = FMath::Max(2, InBranch.OutputWires.Num());
        for (int32 ArmIndex = 0; ArmIndex < ArmCount; ++ArmIndex)
        {
            if (!TryAppendArmTransfer(InBranch, ArmIndex, OutScriptBuilder))
            {
                if (const FBlueprintNode::FWireVariant* const Wire = WireForArm(ArmIndex))
                {
                    RecurseNode(InBranch.FunctionOwner, *Wire, OutScriptBuilder, StopAt);
                }
            }

            if (ArmIndex != ArmCount - 1)
            {
                AppendScript(EndScope, TEXT(""), OutScriptBuilder);
                AppendScript(AddLine, TEXT("else"), OutScriptBuilder);
                AppendScript(StartScope, TEXT(""), OutScriptBuilder);
            }
        }
        AppendScript(EndScope, TEXT(""), OutScriptBuilder);
        return;
    }

    const FBlueprintNode* const FalseArm = InBranch.OutputWires[0].IsType<FBlueprintNode*>() ? InBranch.OutputWires[0].Get<FBlueprintNode*>() : nullptr;
    const FBlueprintNode* const TrueArm = InBranch.OutputWires[1].IsType<FBlueprintNode*>() ? InBranch.OutputWires[1].Get<FBlueprintNode*>() : nullptr;
    const bool bFalseArmEmpty = FalseArm != nullptr && BlueprintScriptFlow::SameKey(*FalseArm, MergeKey);
    const bool bTrueArmEmpty = TrueArm != nullptr && BlueprintScriptFlow::SameKey(*TrueArm, MergeKey);

    AppendScript(StartScope, TEXT(""), OutScriptBuilder);

    if (bFalseArmEmpty)
    {
        // The condition line was written without the `!`: the true arm is the only body.
        RecurseNode(InBranch.FunctionOwner, InBranch.OutputWires[1], OutScriptBuilder, &MergeKey);
        AppendScript(EndScope, TEXT(""), OutScriptBuilder);
    }
    else
    {
        RecurseNode(InBranch.FunctionOwner, InBranch.OutputWires[0], OutScriptBuilder, &MergeKey);
        AppendScript(EndScope, TEXT(""), OutScriptBuilder);

        if (!bTrueArmEmpty)
        {
            AppendScript(AddLine, TEXT("else"), OutScriptBuilder);
            AppendScript(StartScope, TEXT(""), OutScriptBuilder);
            RecurseNode(InBranch.FunctionOwner, InBranch.OutputWires[1], OutScriptBuilder, &MergeKey);
            AppendScript(EndScope, TEXT(""), OutScriptBuilder);
        }
    }

    FBlueprintNode::FWireVariant MergeWire;
    MergeWire.Emplace<FBlueprintNode*>(const_cast<FBlueprintNode*>(Merge));
    RecurseNode(InBranch.FunctionOwner, MergeWire, OutScriptBuilder, StopAt);
}

void FBlueprintScriptVisualizer_Pseudo::AppendLoopTransfer(const bool bContinue, const TWeakObjectPtr<const UFunction>& InOwner, const int64 InJumpOpcode, const int64 InLanding, FStringBuilderBase& OutScriptBuilder)
{
    // The arm's jump opcode is dispatched for both arms, so an address breakpoint on it would fire on every
    // evaluation. The stop waits for the dispatch pair instead; without one the line stays structural,
    // because an approximate stop is worse than none.
    FBlueprintLineInfo Info;
    const UFunction* const Owner = InOwner.Get();
    if (Owner != nullptr && Owner->Script.IsValidIndex(static_cast<int32>(InJumpOpcode)) && Owner->Script.IsValidIndex(static_cast<int32>(InLanding)))
    {
        Info.OwnerFunction = InOwner;
        Info.ScriptIndex = InJumpOpcode;
        Info.TransferTarget = InLanding;
        Info.Kind = EBlueprintLineKind::LoopTransfer;
    }

    PushLocation(OutScriptBuilder, Info);
    OutScriptBuilder.Appendf(TEXT("%s%s;\n"), GetIndentString(), bContinue ? TEXT("continue") : TEXT("break"));
}

bool FBlueprintScriptVisualizer_Pseudo::TryAppendArmTransfer(const FBlueprintNode& InBranch, const int32 ArmIndex, FStringBuilderBase& OutScriptBuilder)
{
    if (InBranch.BackEdgeTarget == INDEX_NONE || InBranch.BackEdgeWire != ArmIndex || LoopStack.Num() == 0)
    {
        return false;
    }

    const FLoopContext& Loop = LoopStack.Last();
    if (InBranch.BackEdgeTarget == Loop.Header.Address)
    {
        AppendLoopTransfer(true, InBranch.FunctionOwner, InBranch.BackEdgeJumpOpcode, InBranch.BackEdgeLanding, OutScriptBuilder);
        return true;
    }

    if (InBranch.BackEdgeTarget == Loop.Exit.Address)
    {
        AppendLoopTransfer(false, InBranch.FunctionOwner, InBranch.BackEdgeJumpOpcode, InBranch.BackEdgeLanding, OutScriptBuilder);
        return true;
    }

    return false;
}

void FBlueprintScriptVisualizer_Pseudo::RenderDoWhileFlow(const BlueprintScriptFlow::FLoop& InLoop, FStringBuilderBase& OutScriptBuilder, const FFlowKey* const StopAt)
{
    if (InLoop.HeaderNode == nullptr || InLoop.ConditionNode == nullptr || InLoop.ConditionNode->OutputWires.Num() < 1)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("RenderDoWhileFlow: incomplete loop shape; the loop is omitted from the rendered text"));
        return;
    }

    AppendScript(AddLine, TEXT("do"), OutScriptBuilder);
    AppendScript(StartScope, TEXT(""), OutScriptBuilder);

    FLoopContext Context{InLoop.Header, InLoop.Exit, InLoop.Latch};
    LoopStack.Push(Context);

    FBlueprintNode::FWireVariant HeaderWire;
    HeaderWire.Emplace<FBlueprintNode*>(const_cast<FBlueprintNode*>(InLoop.HeaderNode));
    RecurseNode(InLoop.HeaderNode->FunctionOwner, HeaderWire, OutScriptBuilder, &InLoop.Condition, false);

    LoopStack.Pop();
    AppendScript(EndScope, TEXT(""), OutScriptBuilder);

    FString Condition;
    for (const FBlueprintNode::FNodeVariant& Input : InLoop.ConditionNode->InputNodes)
    {
        if (!Condition.IsEmpty())
        {
            Condition += TEXT(", ");
        }
        Condition += Visit(*this, Input);
    }
    if (Condition.IsEmpty())
    {
        Condition = TEXT("true");
    }

    // The VM dispatches the tail condition on every iteration, so the `while` line owns the condition
    // statement's address and a breakpoint on that line can arm. The condition is not drawn as a statement
    // of its own, so it is recorded and marked here: a later edge that reaches the node renders a goto
    // instead of a second copy of the same address.
    RecordStatement(*InLoop.ConditionNode, OutScriptBuilder);
    MarkStatementRendered(*InLoop.ConditionNode);
    OutScriptBuilder.Appendf(TEXT("%swhile (%s);\n"), GetIndentString(), *Condition);

    RecurseNode(InLoop.ConditionNode->FunctionOwner, InLoop.ConditionNode->OutputWires[0], OutScriptBuilder, StopAt);
}

void FBlueprintScriptVisualizer_Pseudo::RenderComputedWhileFlow(const BlueprintScriptFlow::FLoop& InLoop, FStringBuilderBase& OutScriptBuilder, const FFlowKey* const StopAt)
{
    if (InLoop.HeaderNode == nullptr || InLoop.ConditionNode == nullptr || InLoop.ConditionNode->OutputWires.Num() < 2)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("RenderComputedWhileFlow: incomplete loop shape; the loop is omitted from the rendered text"));
        return;
    }

    // The branch is part of the loop control, not a statement of its own: the condition line owns the
    // address of the statement that evaluates it, which is what the VM dispatches once per iteration.
    if (!MarkStatementRendered(*InLoop.HeaderNode))
    {
        AppendGoto(InLoop.HeaderNode->FunctionOwner, InLoop.HeaderNode->ScriptIndex, OutScriptBuilder);
        return;
    }

    FoldedBranches.Add(InLoop.ConditionNode);

    FString Condition;
    for (const FBlueprintNode::FNodeVariant& Input : InLoop.HeaderNode->InputNodes)
    {
        if (!Condition.IsEmpty())
        {
            Condition += TEXT(", ");
        }
        Condition += Visit(*this, Input);
    }
    if (Condition.IsEmpty())
    {
        Condition = TEXT("true");
    }

    RecordStatement(*InLoop.HeaderNode, OutScriptBuilder);
    OutScriptBuilder.Appendf(TEXT("%swhile (%s)\n"), GetIndentString(), *Condition);
    AppendScript(StartScope, TEXT(""), OutScriptBuilder);

    FLoopContext Context{InLoop.Header, InLoop.Exit, InLoop.Latch};
    LoopStack.Push(Context);
    RecurseNode(InLoop.HeaderNode->FunctionOwner, InLoop.ConditionNode->OutputWires[1], OutScriptBuilder, StopAt, false);
    LoopStack.Pop();

    AppendScript(EndScope, TEXT(""), OutScriptBuilder);

    RecurseNode(InLoop.ConditionNode->FunctionOwner, InLoop.ConditionNode->OutputWires[0], OutScriptBuilder, StopAt);
}

void FBlueprintScriptVisualizer_Pseudo::AppendScript(const ELineAppendMode InLineAppendMode, const TCHAR* InString, FStringBuilderBase& InOutScriptBuilder) const
{
    const bool bTopLevel = ActiveSourceMap != nullptr && &InOutScriptBuilder == ActiveTopLevelBuilder;

    switch (InLineAppendMode)
    {
    case StartScope:
        if (bTopLevel)
            RecordStructural(InOutScriptBuilder);
        InOutScriptBuilder.Appendf(TEXT("%s{%s\n"), GetIndentString(), InString);
        Indent();
        break;
    case EndScope:
        if (bTopLevel)
            RecordStructural(InOutScriptBuilder);
        Unindent();
        InOutScriptBuilder.Appendf(TEXT("%s}%s\n"), GetIndentString(), InString);
        break;
    case EndScopeNoReturn:
        if (bTopLevel)
            RecordStructural(InOutScriptBuilder);
        Unindent();
        InOutScriptBuilder.Appendf(TEXT("%s}%s"), GetIndentString(), InString);
        break;
    case AddLine:
        if (bTopLevel)
            RecordStructural(InOutScriptBuilder);
        InOutScriptBuilder.Appendf(TEXT("%s%s\n"), GetIndentString(), InString);
        break;
    case StartLine:
        if (bTopLevel)
            RecordStructural(InOutScriptBuilder);
        InOutScriptBuilder.Appendf(TEXT("%s%s"), GetIndentString(), InString);
        break;
    case Append:
        InOutScriptBuilder.Append(InString);
        break;
    case AppendLine:
        InOutScriptBuilder.Appendf(TEXT("%s\n"), InString);
        break;
    }
}

void FBlueprintScriptVisualizer_Pseudo::AppendSequenceBoundary(const FBlueprintNode& InSequence, const FBlueprintNode::FWireVariant& InWire, const int32 WireIndex, FStringBuilderBase& OutScriptBuilder) const
{
    if (ActiveSourceMap != nullptr && &OutScriptBuilder == ActiveTopLevelBuilder)
    {
        FBlueprintLineInfo Info;
        Info.Kind = EBlueprintLineKind::Sequence;
        Info.OwnerFunction = InSequence.FunctionOwner;

        // The boundary is where the pin begins: the target node's first opcode, or the jump address of a
        // wire that resolved to no node. Both are what the engine's sequence pin breaks on.
        if (InWire.IsType<int64>())
        {
            Info.ScriptIndex = InWire.Get<int64>();
        }
        else if (InWire.IsType<FBlueprintNode*>())
        {
            if (const FBlueprintNode* const Target = InWire.Get<FBlueprintNode*>())
            {
                Info.ScriptIndex = Target->ScriptIndex;
                if (Target->FunctionOwner.IsValid())
                {
                    Info.OwnerFunction = Target->FunctionOwner;
                }
            }
        }

        // EndOfScript and a missing owner have no opcode to patch, so the line stays structural.
        const UFunction* const Owner = Info.OwnerFunction.Get();
        if (Owner == nullptr || !Owner->Script.IsValidIndex(static_cast<int32>(Info.ScriptIndex)))
        {
            Info.Kind = EBlueprintLineKind::None;
        }
        PushLocation(OutScriptBuilder, Info);
    }

    OutScriptBuilder.Appendf(TEXT("%s// Sequence %d.\n"), GetIndentString(), WireIndex);
}

void FBlueprintScriptVisualizer_Pseudo::AppendGoto(const TWeakObjectPtr<const UFunction>& InOwner, const int64 InAddress, FStringBuilderBase& OutScriptBuilder) const
{
    if (ActiveSourceMap != nullptr && &OutScriptBuilder == ActiveTopLevelBuilder)
    {
        FBlueprintLineInfo Info;
        Info.Kind = EBlueprintLineKind::Goto;
        Info.OwnerFunction = InOwner;
        Info.ScriptIndex = InAddress;

        // A target outside the script has no opcode to patch, so the line stays structural.
        const UFunction* const Owner = InOwner.Get();
        if (Owner == nullptr || !Owner->Script.IsValidIndex(static_cast<int32>(InAddress)))
        {
            Info.Kind = EBlueprintLineKind::None;
        }
        PushLocation(OutScriptBuilder, Info);
    }

    OutScriptBuilder.Appendf(TEXT("%sgoto _0x%.3llX;\n"), GetIndentString(), InAddress);
}

void FBlueprintScriptVisualizer_Pseudo::RecurseNode(const TWeakObjectPtr<const UFunction>& InFunction, const FBlueprintNode::FWireVariant& InBlueprintNode, FStringBuilderBase& OutScriptBuilder, const FFlowKey* const StopAt, const bool bAllowLoopWrap)
{
    if (!InBlueprintNode.IsType<FBlueprintNode*>())
    {
        AppendGoto(InFunction, InBlueprintNode.Get<int64>(), OutScriptBuilder);
        return;
    }

    const FBlueprintNode* const PointerNode = InBlueprintNode.Get<FBlueprintNode*>();
    if (PointerNode == nullptr)
    {
        // The variant reports the pointer alternative, not a non-null pointer.
        return;
    }

    const FBlueprintNode& BlueprintNode = *PointerNode;
    const EExprToken OpCode = BlueprintNode.Opcode;
    bool IsScoped = false;
    bool bSkipInputNodeLoop = false;
    bool bMoveWorldContextToEnd = false;

    FString WideString;
    FString Suffix;

    auto InputNode = BlueprintNode.InputNodes.CreateConstIterator();

    // EX_PopExecutionFlow hands control back to the address pushed by EX_PushExecutionFlow, so it is not a
    // statement of its own. EX_Return is a terminator too, but it carries a value, so it is rendered below.
    if (OpCode == EX_PopExecutionFlow)
    {
        return;
    }

    if (StopAt != nullptr && BlueprintScriptFlow::SameKey(BlueprintNode, *StopAt))
    {
        return;
    }

    // An edge inside a loop body that reaches the loop's exit is a break: the exit itself is rendered after
    // the loop, not here. The break owns no node of its own, so the pair comes from the jump that landed on
    // this node; without one (an edge the walk reached without a jump) the line stays structural.
    if (LoopStack.Num() > 0 && BlueprintScriptFlow::SameKey(BlueprintNode, LoopStack.Last().Exit))
    {
        AppendLoopTransfer(false, BlueprintNode.FunctionOwner, BlueprintNode.ArrivalJumpOpcode, BlueprintNode.ArrivalLanding, OutScriptBuilder);
        return;
    }

    // A computed-condition while loop starts on the statement the compiler used for the condition: it
    // stores the comparison result and the branch that follows reads it. The latch returns to that
    // statement, so the condition has to be drawn as the `while` header before the statement is rendered.
    if (bAllowLoopWrap && !WrappedLoops.Contains(&BlueprintNode))
    {
        BlueprintScriptFlow::FLoop ComputedLoop;
        bool bHasComputedLoop = false;
        if (!CheckedComputedWhile.Contains(&BlueprintNode))
        {
            CheckedComputedWhile.Add(&BlueprintNode);
            bHasComputedLoop = BlueprintScriptFlow::FindComputedWhileLoop(BlueprintNode, ComputedLoop);
            if (bHasComputedLoop)
            {
                ComputedWhileLoops.Add(&BlueprintNode, ComputedLoop);
            }
        }
        else if (const BlueprintScriptFlow::FLoop* const Cached = ComputedWhileLoops.Find(&BlueprintNode))
        {
            ComputedLoop = *Cached;
            bHasComputedLoop = true;
        }

        if (bHasComputedLoop)
        {
            WrappedLoops.Add(&BlueprintNode);
            RenderComputedWhileFlow(ComputedLoop, OutScriptBuilder, StopAt);
            return;
        }
    }

    // A do/while body starts on an ordinary statement, so it is recognised here, before the statement is
    // drawn: the body runs to the tail condition, which is rendered as `while (...)`.
    if (bAllowLoopWrap && !WrappedLoops.Contains(&BlueprintNode))
    {
        BlueprintScriptFlow::FLoop Loop;
        bool bHasLoop = false;
        if (!CheckedDoWhile.Contains(&BlueprintNode))
        {
            CheckedDoWhile.Add(&BlueprintNode);
            bHasLoop = BlueprintScriptFlow::FindDoWhileLoop(BlueprintNode, Loop);
            if (bHasLoop)
            {
                DoWhileLoops.Add(&BlueprintNode, Loop);
            }
        }
        else if (const BlueprintScriptFlow::FLoop* const Cached = DoWhileLoops.Find(&BlueprintNode))
        {
            Loop = *Cached;
            bHasLoop = true;
        }

        if (bHasLoop)
        {
            WrappedLoops.Add(&BlueprintNode);
            RenderDoWhileFlow(Loop, OutScriptBuilder, StopAt);
            return;
        }
    }

    // A branch target that is also another arm's fallthrough reaches the same bytecode from a second edge.
    // Draw the statement once and make the later edge a transfer to it, so one line maps to one address.
    if (OpCode != EX_PushExecutionFlow && OpCode != EX_Max && !MarkStatementRendered(BlueprintNode))
    {
        AppendGoto(BlueprintNode.FunctionOwner, RecordedStatementAddress(BlueprintNode), OutScriptBuilder);
        return;
    }

    if (OpCode != EX_PushExecutionFlow && OpCode != EX_Max)
    {
        RecordStatement(BlueprintNode, OutScriptBuilder);
    }

    // A linear statement whose jump returns to the loop header is an early continue. The natural latch does
    // the same jump but needs no line; branch arms are handled by RenderBranchFlow.
    const bool bLinearContinue = LoopStack.Num() > 0 && OpCode != EX_JumpIfNot && OpCode != EX_PopExecutionFlowIfNot && BlueprintNode.BackEdgeTarget != INDEX_NONE && BlueprintNode.BackEdgeTarget == LoopStack.Last().Header.Address && !BlueprintScriptFlow::SameKey(BlueprintNode, LoopStack.Last().Latch);

    // Bytecode branch, before the line is written: a branch whose latch jumps back to it is a while loop,
    // and a branch whose false arm begins at the merge is written with the condition inverted so the empty
    // arm is not emitted. A branch arm that jumps back is a transfer, so it never has a merge.
    bool bBranchLoop = false;
    bool bBranchInverted = false;
    const bool bBranchArmTransfer = OpCode == EX_JumpIfNot && BlueprintNode.BackEdgeTarget != INDEX_NONE && (BlueprintNode.BackEdgeWire == 0 || BlueprintNode.BackEdgeWire == 1);
    if (OpCode == EX_JumpIfNot && BlueprintNode.OutputWires.Num() >= 2)
    {
        bBranchLoop = BlueprintScriptFlow::IsLoopHeader(BlueprintNode);
        if (!bBranchLoop && !bBranchArmTransfer)
        {
            const FBlueprintNode* Merge = nullptr;
            FFlowKey MergeKey;
            const FBlueprintNode* const FalseArm = BlueprintNode.OutputWires[0].IsType<FBlueprintNode*>() ? BlueprintNode.OutputWires[0].Get<FBlueprintNode*>() : nullptr;
            bBranchInverted = FalseArm != nullptr && BlueprintScriptFlow::FindMerge(BlueprintNode, Merge, MergeKey) && BlueprintScriptFlow::SameKey(*FalseArm, MergeKey);
        }
    }

    switch (OpCode)
    {
    case EX_Return:
    {
        // A void function encodes EX_Nothing as the value (KismetCompilerVMBackend::EmitReturn), which has
        // no useful rendering, so drop it and emit a bare return.
        if (InputNode && InputNode->IsType<EExprToken>() && InputNode->Get<EExprToken>() == EX_Nothing)
        {
            ++InputNode;
        }

        if (InputNode)
        {
            OutScriptBuilder.Appendf(TEXT("%sreturn "), GetIndentString());
        }
        else
        {
            OutScriptBuilder.Appendf(TEXT("%sreturn"), GetIndentString());
        }
        Suffix = ";";
    }
    break;

    case EX_SetSet:
    case EX_SetMap:
        if (BlueprintNode.ContainerLiteral.Kind != EBlueprintContainerKind::None)
        {
            OutScriptBuilder.Appendf(TEXT("%s%s = "), GetIndentString(), *operator()(BlueprintNode.ReturnNode));
            OutScriptBuilder.Append(*RenderContainerLiteral(BlueprintNode));
            Suffix = ";";
            bSkipInputNodeLoop = true;
        }
        else
        {
            OutScriptBuilder.Appendf(TEXT("%s%s = {"), GetIndentString(), *operator()(BlueprintNode.ReturnNode));
            Suffix = "};";
        }
        break;

    // EX_SetArray wraps a MakeArray operand; the typed literal is on that operand, so the ordinary input
    // loop renders it and this node only supplies the destination.
    case EX_SetArray:
    case EX_Let:
    case EX_LetObj:
    case EX_LetWeakObjPtr:
    case EX_LetBool:
    case EX_LetDelegate:
    case EX_LetMulticastDelegate:
    case EX_LetValueOnPersistentFrame:
        OutScriptBuilder.Appendf(TEXT("%s%s = "), GetIndentString(), *operator()(BlueprintNode.ReturnNode));
        Suffix = ";";
        break;

    case EX_BindDelegate:
        OutScriptBuilder.Appendf(TEXT("%s%s.Bind("), GetIndentString(), *operator()(BlueprintNode.ReturnNode));
        Suffix = ");";
        break;

    case EX_AddMulticastDelegate:
        OutScriptBuilder.Appendf(TEXT("%s%s.Add("), GetIndentString(), *operator()(BlueprintNode.ReturnNode));
        Suffix = ");";
        break;

    case EX_RemoveMulticastDelegate:
        OutScriptBuilder.Appendf(TEXT("%s%s.Remove("), GetIndentString(), *operator()(BlueprintNode.ReturnNode));
        Suffix = ");";
        break;

    case EX_ClearMulticastDelegate:
        OutScriptBuilder.Appendf(TEXT("%s%s.Clear()"), GetIndentString(), *operator()(BlueprintNode.ReturnNode));
        Suffix = ";";
        break;

    case EX_JumpIfNot:
        OutScriptBuilder.Appendf(TEXT("%s%s ("), GetIndentString(), bBranchLoop ? TEXT("while") : TEXT("if"));
        if (!bBranchLoop && !bBranchInverted)
        {
            OutScriptBuilder.Append(TEXT("!"));
        }
        Suffix = ")";
        IsScoped = true;
        break;

    case EX_PopExecutionFlowIfNot:
        OutScriptBuilder.Appendf(TEXT("%sif ("), GetIndentString());
        Suffix = ")";
        IsScoped = true;
        break;

    case EX_CallMath:
    case EX_FinalFunction:
    case EX_VirtualFunction:
    case EX_LocalFinalFunction:
    case EX_LocalVirtualFunction:
        bMoveWorldContextToEnd = BlueprintNode.bCalleeHasWorldContext;
        OutScriptBuilder.Appendf(TEXT("%s"), GetIndentString());
        AppendReceiver(BlueprintNode, OutScriptBuilder);
        WideString = SanitiseIdentifier(BlueprintNode.Name.ToString());
        OutScriptBuilder.Appendf(TEXT("%s("), *WideString);
        Suffix = TEXT(");");
        break;

    case EX_CallMulticastDelegate:
        OutScriptBuilder.Appendf(TEXT("%s"), GetIndentString());
        AppendReceiver(BlueprintNode, OutScriptBuilder);
        // The leading input is the delegate being broadcast, the remainder are its parameters.
        if (InputNode)
        {
            OutScriptBuilder.Appendf(TEXT("%s"), *Visit(*this, *InputNode++));
        }
        else
        {
            OutScriptBuilder.Appendf(TEXT("%s"), *SanitiseIdentifier(BlueprintNode.Name.ToString()));
        }
        OutScriptBuilder.Append(TEXT(".Broadcast("));
        Suffix = TEXT(");");
        break;

    case EX_PushExecutionFlow:
    case EX_Max:
        break;

    default:
    {
        WideString = SanitiseIdentifier(BlueprintNode.Name.ToString());
        OutScriptBuilder.Appendf(TEXT("%s%s("), GetIndentString(), *WideString);
        Suffix = TEXT(");");
    }
    break;
    }

    if (!bSkipInputNodeLoop)
    {
        TArray<FString> Parameters;
        for (; InputNode; ++InputNode)
        {
            FString Parameter = InputNode.GetIndex() == 0 ? RenderEnumAssignment(BlueprintNode, *InputNode) : FString();
            if (Parameter.IsEmpty())
            {
                Parameter = Visit(*this, *InputNode);
            }
            Parameters.Add(MoveTemp(Parameter));
        }

        // A fresh compile emits the WorldContext self argument first; the golden convention (and the
        // saved reference assets) put it last. Move a leading `this` to the end so the rendered text
        // does not depend on the engine's argument order.
        if (bMoveWorldContextToEnd && Parameters.Num() > 1 && Parameters[0] == TEXT("this"))
        {
            FString Self = Parameters[0];
            Parameters.RemoveAt(0);
            Parameters.Add(MoveTemp(Self));
        }

        for (int32 Index = 0; Index < Parameters.Num(); ++Index)
        {
            OutScriptBuilder.Appendf(TEXT("%s%s"), *Parameters[Index], Index >= Parameters.Num() - 1 ? TEXT("") : TEXT(", "));
        }
    }

    if (!Suffix.IsEmpty())
    {
        OutScriptBuilder.Appendf(TEXT("%s\n"), *Suffix);
    }

    // A branch owns its scopes: RenderBranchFlow opens and closes them around the arms and the merge.
    const bool bStructuredBranch = OpCode == EX_JumpIfNot && (BlueprintNode.OutputWires.Num() >= 2 || bBranchArmTransfer);
    if (bLinearContinue)
    {
        AppendLoopTransfer(true, BlueprintNode.FunctionOwner, BlueprintNode.BackEdgeJumpOpcode, BlueprintNode.BackEdgeLanding, OutScriptBuilder);
        return;
    }

    if (IsScoped && !bStructuredBranch)
    {
        AppendScript(StartScope, TEXT(""), OutScriptBuilder);
    }

    if (bStructuredBranch)
    {
        RenderBranchFlow(BlueprintNode, OutScriptBuilder, StopAt);
        return;
    }

    for (auto WireNode = BlueprintNode.OutputWires.CreateConstIterator(); WireNode; ++WireNode)
    {
        if (OpCode == EX_PushExecutionFlow)
        {
            // Sequence pins are linear continuations, not alternative edges. An empty pin can fold into the
            // next one, and a branch can converge on a later pin, so a pin may resolve to bytecode an
            // earlier pin already drew. It is already in the output: drop it rather than transfer to it.
            if (WireNode->IsType<FBlueprintNode*>() && WireNode->Get<FBlueprintNode*>() != nullptr && HasRenderedStatement(*WireNode->Get<FBlueprintNode*>()))
            {
                continue;
            }

            AppendSequenceBoundary(BlueprintNode, *WireNode, WireNode.GetIndex(), OutScriptBuilder);
        }

        RecurseNode(BlueprintNode.FunctionOwner, *WireNode, OutScriptBuilder, StopAt);
    }

    if (IsScoped)
    {
        AppendScript(EndScope, TEXT(""), OutScriptBuilder);
    }
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const UObject* const& Value) const
{
    return Value ? *Value->GetName() : TEXT("");
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const FName& Value) const
{
    const FString WideString{Value.ToString()};
    return FString::Printf(TEXT("\"%s\""), *WideString);
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const FDelegateLiteral& Value) const
{
    return FString::Printf(TEXT("FScriptDelegate{\"%s\"}"), *Value.FunctionName.ToString());
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const int64& Value) const
{
    return FString::Printf(TEXT("%lld"), Value);
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const uint64& Value) const
{
    return FString::Printf(TEXT("%llu"), Value);
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const double& Value) const
{
    return FString::Printf(TEXT("%.4f"), Value);
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const FVector* const& Value) const
{
    return !Value ? TEXT("FVector(Invalid)") : FString::Printf(TEXT("FVector(%.4f, %.4f, %.4f)"), Value->X, Value->Y, Value->Z);
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const FRotator* const& Value) const
{
    return !Value ? TEXT("FRotator(Invalid)") : FString::Printf(TEXT("FRotator(%.4f, %.4f, %.4f)"), Value->Pitch, Value->Yaw, Value->Roll);
}

#if BI_ENGINE_AT_LEAST_5
FString FBlueprintScriptVisualizer_Pseudo::operator()(const BI_FVector3f* const& Value) const
{
    return !Value ? TEXT("FVector3f(Invalid)") : FString::Printf(TEXT("FVector3f(%.4f, %.4f, %.4f)"), Value->X, Value->Y, Value->Z);
}
#endif

FString FBlueprintScriptVisualizer_Pseudo::operator()(const FTransform* const& Value) const
{
    if (!Value)
    {
        return TEXT("FTransform(Invalid)");
    }

    const FVector Translation = Value->GetTranslation();
    const FRotator Rotation = Value->Rotator();
    const FVector Scale = Value->GetScale3D();
    // FTransform::Rotator() can return a negative zero on 5.8 (the quaternion conversion produces
    // -0.0), which would make the pseudo-source text depend on the engine's float path. Render any
    // zero component as +0.0 so the golden output stays stable across engine versions.
    const auto FixedZero = [](const double Component) { return Component == 0.0 ? 0.0 : Component; };
    return FString::Printf(TEXT("FTransform(T=(%.4f, %.4f, %.4f), R=(%.4f, %.4f, %.4f), S=(%.4f, %.4f, %.4f))"), FixedZero(Translation.X), FixedZero(Translation.Y), FixedZero(Translation.Z), FixedZero(Rotation.Pitch), FixedZero(Rotation.Yaw), FixedZero(Rotation.Roll), FixedZero(Scale.X), FixedZero(Scale.Y), FixedZero(Scale.Z));
}

FString FBlueprintScriptVisualizer_Pseudo::RenderEnumAssignment(const FBlueprintNode& InNode, const FBlueprintNode::FNodeVariant& InInput) const
{
    if (InNode.Opcode != EX_Let || InNode.ReturnNode.Property == nullptr)
    {
        return FString();
    }

    const UEnum* Enum = nullptr;
    if (const FByteProperty* const ByteProperty = CastField<FByteProperty>(InNode.ReturnNode.Property))
    {
        Enum = ByteProperty->Enum;
    }
    else if (const FEnumProperty* const EnumProperty = CastField<FEnumProperty>(InNode.ReturnNode.Property))
    {
        Enum = EnumProperty->GetEnum();
    }

    if (Enum == nullptr)
    {
        return FString();
    }

    // The name the Blueprint graph shows for the value, which is the enumerant's authored display name. It
    // is read as the display text's *source* string, not the text itself: UEnum builds a native enum's
    // display text inside the UObjectDisplayNames localization namespace, so ToString() would follow the
    // editor culture. A user-defined enum carries its display names in DisplayNameMap, a plain UPROPERTY
    // on a runtime class, so the same name is there in a packaged build. Authored names may contain
    // characters that are not identifiers, so the result still goes through SanitiseIdentifier.
    const auto RenderValue = [Enum](const int64 InValue) -> FString {
        // Hold the FText: the source string points into its history, which the temporary would own.
        const FText DisplayText = Enum->GetDisplayNameTextByValue(InValue);
        if (const FString* const Source = FTextInspector::GetSourceString(DisplayText))
        {
            if (!Source->IsEmpty())
            {
                return SanitiseIdentifier(*Source);
            }
        }

        // No display text at all: fall back to the enumerant's own name, and render nothing for a value
        // that is not in the enum.
        const FString EnumerantName = Enum->GetNameStringByValue(InValue);
        return EnumerantName.IsEmpty() ? FString() : SanitiseIdentifier(EnumerantName);
    };

    // EX_ByteConst stores a uint64; EX_IntConst and EX_Int64Const store an int64.
    if (const uint64* const ByteValue = InInput.TryGet<uint64>())
    {
        return RenderValue(static_cast<int64>(*ByteValue));
    }
    if (const int64* const IntValue = InInput.TryGet<int64>())
    {
        return RenderValue(*IntValue);
    }

    return FString();
}

FString FBlueprintScriptVisualizer_Pseudo::RenderContainerLiteral(const FBlueprintNode& InNode) const
{
    const FBlueprintContainerLiteral& Literal = InNode.ContainerLiteral;
    if (Literal.Kind == EBlueprintContainerKind::None)
    {
        return FString();
    }

    const auto TypeName = [this](const FProperty* const Property) -> FString {
        return Property != nullptr ? GetString(Property) : FString();
    };

    TStringBuilder<LiteralBuilderCapacity> Builder;
    switch (Literal.Kind)
    {
    case EBlueprintContainerKind::Array:
        Builder.Append(TEXT("TArray<"));
        Builder.Append(*TypeName(Literal.ElementType));
        Builder.Append(TEXT(">{"));
        break;
    case EBlueprintContainerKind::Set:
        Builder.Append(TEXT("TSet<"));
        Builder.Append(*TypeName(Literal.ElementType));
        Builder.Append(TEXT(">{"));
        break;
    case EBlueprintContainerKind::Map:
        Builder.Append(TEXT("TMap<"));
        Builder.Append(*TypeName(Literal.ElementType));
        Builder.Append(TEXT(", "));
        Builder.Append(*TypeName(Literal.ValueType));
        Builder.Append(TEXT(">{"));
        break;
    default:
        return FString();
    }

    if (Literal.Kind == EBlueprintContainerKind::Map)
    {
        // InputNodes holds key, value, key, value…; the literal pairs them rather than printing a flat list.
        const int32 PairCount = FMath::Min(Literal.Count, InNode.InputNodes.Num() / 2);
        for (int32 Pair = 0; Pair < PairCount; ++Pair)
        {
            if (Pair > 0)
            {
                Builder.Append(TEXT(", "));
            }
            Builder.Append(TEXT("{"));
            Builder.Append(*Visit(*this, InNode.InputNodes[Pair * 2]));
            Builder.Append(TEXT(", "));
            Builder.Append(*Visit(*this, InNode.InputNodes[Pair * 2 + 1]));
            Builder.Append(TEXT("}"));
        }
    }
    else
    {
        const int32 ElementCount = FMath::Min(Literal.Count, InNode.InputNodes.Num());
        for (int32 Index = 0; Index < ElementCount; ++Index)
        {
            if (Index > 0)
            {
                Builder.Append(TEXT(", "));
            }
            Builder.Append(*Visit(*this, InNode.InputNodes[Index]));
        }
    }

    Builder.Append(TEXT("}"));
    return Builder.ToString();
}

void FBlueprintScriptVisualizer_Pseudo::AppendReceiver(const FBlueprintNode& InNode, FStringBuilderBase& OutScriptBuilder) const
{
    if (InNode.Receiver.IsSet() && !IsStaticReceiver(*InNode.Receiver))
    {
        OutScriptBuilder.Appendf(TEXT("%s->"), *Visit(*this, *InNode.Receiver));
    }
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(FBlueprintNode* const& Value) const
{
    if (Value == nullptr)
    {
        return FString();
    }

    if (Value->ContainerLiteral.Kind != EBlueprintContainerKind::None)
    {
        return RenderContainerLiteral(*Value);
    }

    FString Suffix;
    TStringBuilder<SwitchBuilderCapacity> StringBuilder;
    auto InputNode = Value->InputNodes.CreateConstIterator();

    AppendReceiver(*Value, StringBuilder);

    switch (Value->Opcode)
    {
#if BI_ENGINE_AT_LEAST_5
    case EX_Cast:
    {
        const uint64* const CastTypePtr = InputNode ? InputNode->TryGet<uint64>() : nullptr;
        if (CastTypePtr == nullptr)
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Visualizer: EX_Cast without a conversion operand at offset %lld"), Value->ScriptIndex);
            break;
        }

        const uint64 CastType = *CastTypePtr;
        ++InputNode;

        switch (CastType)
        {
        case CST_ObjectToInterface:
        {
            const UObject* ObjectOperand = nullptr;
            if (InputNode)
            {
                ObjectOperand = GetObjectOperand(*InputNode++);
            }
            StringBuilder.Appendf(TEXT("Cast<%s>("), *GetCastTypeName(ObjectOperand, true));
            Suffix = ")";
        }
        break;
        case CST_ObjectToBool:
        case CST_InterfaceToBool:
            Suffix = " != nullptr";
            break;
        case CST_DoubleToFloat:
            StringBuilder.Appendf(TEXT("Cast<float>("));
            Suffix = ")";
            break;
        case CST_FloatToDouble:
            StringBuilder.Appendf(TEXT("Cast<double>("));
            Suffix = ")";
            break;
        default:
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Visualizer: EX_Cast with an unknown conversion 0x%llX at offset %lld"), CastType, Value->ScriptIndex);
            break;
        }
    }
    break;
#endif
#if !BI_ENGINE_AT_LEAST_5
    case EX_PrimitiveCast:
    {
        const uint64* const CastTypePtr = InputNode ? InputNode->TryGet<uint64>() : nullptr;
        if (CastTypePtr == nullptr)
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Visualizer: EX_PrimitiveCast without a conversion operand at offset %lld"), Value->ScriptIndex);
            break;
        }

        const uint64 CastType = *CastTypePtr;
        ++InputNode;

        switch (CastType)
        {
        case 0x46: // CST_ObjectToInterface
        {
            const UObject* ObjectOperand = nullptr;
            if (InputNode)
            {
                ObjectOperand = GetObjectOperand(*InputNode++);
            }
            StringBuilder.Appendf(TEXT("Cast<%s>("), *GetCastTypeName(ObjectOperand, true));
            Suffix = ")";
            break;
        }
        case 0x47: // CST_ObjectToBool
        case 0x49: // CST_InterfaceToBool
            Suffix = " != nullptr";
            break;
        default:
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Visualizer: EX_PrimitiveCast with an unknown conversion 0x%llX at offset %lld"), CastType, Value->ScriptIndex);
            break;
        }
    }
    break;
#endif

    case EX_CallMath:
    case EX_FinalFunction:
    case EX_VirtualFunction:
    case EX_LocalVirtualFunction:
    case EX_LocalFinalFunction:
    case EX_StructConst:
    case EX_ArrayConst:
    case EX_SetConst:
    case EX_MapConst:
    case EX_SetArray:
    {
        const FString WideString{SanitiseIdentifier(Value->Name.ToString())};
        StringBuilder.Appendf(TEXT("%s("), *WideString);
        Suffix = ")";
    }
    break;

    case EX_SwitchValue:
    {
        if (!InputNode)
        {
            StringBuilder.Appendf(TEXT("%s()"), *SanitiseIdentifier(Value->Name.ToString()));
            Suffix = "";
            break;
        }

        const FString SwitchVariableName = *Visit(*this, *InputNode++);
        if (IsIdentifierName(SwitchVariableName))
        {
            AppendScript(AppendLine, *FString::Printf(TEXT("[&%s]()"), *SwitchVariableName), StringBuilder);
        }
        else
        {
            AppendScript(AppendLine, TEXT("[&]()"), StringBuilder);
        }

        AppendScript(StartScope, TEXT(""), StringBuilder);
        AppendScript(AddLine, *FString::Printf(TEXT("switch (%s)"), *SwitchVariableName), StringBuilder);
        AppendScript(StartScope, TEXT(""), StringBuilder);
        // The body consumes two operands per iteration (the case value and its body), so require both.
        while (InputNode.GetIndex() + 1 < Value->InputNodes.Num())
        {
            AppendScript(AddLine, *FString::Printf(TEXT("case %s:"), *Visit(*this, *InputNode++)), StringBuilder);
            AppendScript(AddLine, *FString::Printf(TEXT("\treturn %s;"), *Visit(*this, *InputNode++)), StringBuilder);
        }
        AppendScript(AddLine, *FString::Printf(TEXT("default:")), StringBuilder);
        if (InputNode)
        {
            AppendScript(AddLine, *FString::Printf(TEXT("\treturn %s;"), *Visit(*this, *InputNode++)), StringBuilder);
        }
        else
        {
            AppendScript(AddLine, TEXT("\treturn <none>;"), StringBuilder);
        }
        AppendScript(EndScope, TEXT(""), StringBuilder);
        AppendScript(EndScopeNoReturn, TEXT("()"), StringBuilder);
        Suffix = "";
    }
    break;

    case EX_DynamicCast:
    case EX_ObjToInterfaceCast:
    case EX_CrossInterfaceCast:
    case EX_MetaCast:
    case EX_InterfaceToObjCast:
    {
        const bool bInterfaceCast = Value->Opcode == EX_ObjToInterfaceCast || Value->Opcode == EX_CrossInterfaceCast;
        const UObject* ObjectOperand = nullptr;
        if (InputNode)
        {
            ObjectOperand = GetObjectOperand(*InputNode++);
        }
        StringBuilder.Appendf(TEXT("Cast<%s>("), *GetCastTypeName(ObjectOperand, bInterfaceCast));
        Suffix = ")";
    }
    break;

    case EX_Max:
        break;

    default:
    {
        const FString WideString{SanitiseIdentifier(Value->Name.ToString())};
        StringBuilder.Appendf(TEXT("%s("), *WideString);
        Suffix = ")";
    }
    break;
    }

    for (; InputNode; ++InputNode)
    {
        FString Parameter = Visit(*this, *InputNode);
        StringBuilder.Appendf(TEXT("%s%s"), *Parameter, InputNode.GetIndex() >= Value->InputNodes.Num() - 1 ? TEXT("") : TEXT(", "));
    }

    StringBuilder.Append(Suffix);
    return StringBuilder.ToString();
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const FContextProperty& Value) const
{
    FString String;
    if (Value.Context)
    {
        String.Appendf(TEXT("%s->"), *SanitiseIdentifier(Value.Context->GetName()));
    }
    if (Value.Property)
    {
        String.Appendf(TEXT("%s"), *SanitiseIdentifier(Value.Property->GetName()));
    }
    return String;
}

FString FBlueprintScriptVisualizer_Pseudo::operator()(const EExprToken& Value) const
{
    switch (Value)
    {
    case EX_IntZero:
        return "0";
    case EX_IntOne:
        return "1";
    case EX_True:
        return "true";
    case EX_False:
        return "false";
    case EX_Self:
        return "this";
    case EX_NoObject:
        return "nullptr";
    default:
        return FString::Printf(TEXT("EExprToken(0x%.2X)"), Value);
    }
}