#pragma once
#include "BlueprintInspectorPort.h"
#include "BlueprintScriptDisassembler.h"
#include "BlueprintScriptFlow.h"
#include "BlueprintSourceMap.h"
#include "BlueprintSourceMapBuilder.h"

// UE 5.8 moved StringBuilder back to Misc/; UE5.0-5.7 uses Containers/. Pick whichever exists.
#if __has_include("Containers/StringBuilder.h")
#include "Containers/StringBuilder.h"
#else
#include "Misc/StringBuilder.h"
#endif

class FBlueprintScriptVisualizer_Pseudo
{
public:
    // One instance renders one function at a time: Visualize() resets all per-render state at entry and
    // the const operand visitors mutate that state, so the class is neither reentrant nor thread-safe.
    // Copying/moving is deleted because every meaningful member is borrowed or per-render state.
    FBlueprintScriptVisualizer_Pseudo() = default;
    FBlueprintScriptVisualizer_Pseudo(const FBlueprintScriptVisualizer_Pseudo&) = delete;
    FBlueprintScriptVisualizer_Pseudo& operator=(const FBlueprintScriptVisualizer_Pseudo&) = delete;
    FBlueprintScriptVisualizer_Pseudo(FBlueprintScriptVisualizer_Pseudo&&) = delete;
    FBlueprintScriptVisualizer_Pseudo& operator=(FBlueprintScriptVisualizer_Pseudo&&) = delete;

    void Visualize(const TWeakObjectPtr<const UFunction>& InFunction, FBlueprintScriptDisassembler& InDisassembler, FString& OutScript, FBlueprintSourceMap* OutMap = nullptr);

    FString operator()(const UObject* const& Value) const;
    FString operator()(const FContextProperty& Value) const;
    FString operator()(const EExprToken& Value) const;
    FString operator()(const FName& Value) const;
    FString operator()(const FDelegateLiteral& Value) const;
    FString operator()(const int64& Value) const;
    FString operator()(const uint64& Value) const;
    FString operator()(const double& Value) const;
    FString operator()(const FVector* const& Value) const;
    FString operator()(const FRotator* const& Value) const;
#if BI_ENGINE_AT_LEAST_5
    FString operator()(const BI_FVector3f* const& Value) const;
#endif
    FString operator()(const FTransform* const& Value) const;
    FString operator()(FBlueprintNode* const& Value) const;

private:
    // The structurer's statement identity, so a RenderBranchFlow stop point is the same key the graph
    // analysis compares.
    using FFlowKey = BlueprintScriptFlow::FFlowKey;

    struct FLoopContext
    {
        FFlowKey Header;
        FFlowKey Exit;
        FFlowKey Latch;
    };
    TArray<FLoopContext> LoopStack;
    // Per-render do/while detection cache, keyed by node because the entry node shares address 0 with the
    // first statement. The analysis walks the graph, so run it once per header.
    TMap<const FBlueprintNode*, BlueprintScriptFlow::FLoop> DoWhileLoops;
    TSet<const FBlueprintNode*> CheckedDoWhile;
    // A computed-condition while loop is recognised on its condition statement, which is not a branch.
    TMap<const FBlueprintNode*, BlueprintScriptFlow::FLoop> ComputedWhileLoops;
    TSet<const FBlueprintNode*> CheckedComputedWhile;
    TSet<const FBlueprintNode*> WrappedLoops;
    // Branches folded into loop control (a computed condition). They own no line, so the coverage tests
    // must not expect one.
    TSet<const FBlueprintNode*> FoldedBranches;

public:
    // Valid only until the next Visualize() call: the set is reset at the start of every render.
    [[nodiscard]] const TSet<const FBlueprintNode*>& GetFoldedBranches() const { return FoldedBranches; }

    // Blueprint names may contain spaces and punctuation; rendered code needs identifiers. Public because
    // the navigation model depends on this exact mapping, not just the renderer.
    static FString SanitiseIdentifier(const FString& Value);

private:
    void RecurseNode(const TWeakObjectPtr<const UFunction>& InFunction, const FBlueprintNode::FWireVariant& InBlueprintNode, FStringBuilderBase& OutScriptBuilder, const FFlowKey* StopAt = nullptr, bool bAllowLoopWrap = true);

    void RenderBranchFlow(const FBlueprintNode& InBranch, FStringBuilderBase& OutScriptBuilder, const FFlowKey* StopAt);

    void RenderDoWhileFlow(const BlueprintScriptFlow::FLoop& InLoop, FStringBuilderBase& OutScriptBuilder, const FFlowKey* StopAt);

    void RenderComputedWhileFlow(const BlueprintScriptFlow::FLoop& InLoop, FStringBuilderBase& OutScriptBuilder, const FFlowKey* StopAt);

    // Emits `break;`/`continue;` and records the arm's dispatch pair so the line can be armed. A transfer
    // with no jump record (an edge that reaches the loop exit as a plain node) stays structural.
    void AppendLoopTransfer(bool bContinue, const TWeakObjectPtr<const UFunction>& InOwner, int64 InJumpOpcode, int64 InLanding, FStringBuilderBase& OutScriptBuilder);

    bool TryAppendArmTransfer(const FBlueprintNode& InBranch, int32 ArmIndex, FStringBuilderBase& OutScriptBuilder);

    enum ELineAppendMode : uint8;
    void AppendScript(const ELineAppendMode InLineAppendMode, const TCHAR* InString, FStringBuilderBase& InOutScriptBuilder) const;

    // Emits one `// Sequence N.` boundary and records the bytecode the pin starts at, so the line can arm
    // a breakpoint. A boundary with no executable address stays structural.
    void AppendSequenceBoundary(const FBlueprintNode& InSequence, const FBlueprintNode::FWireVariant& InWire, int32 WireIndex, FStringBuilderBase& OutScriptBuilder) const;

    // Emits one `goto _0x…;` transfer and records the statement address it transfers to, so the line can
    // arm that statement. A transfer with no executable address stays structural.
    void AppendGoto(const TWeakObjectPtr<const UFunction>& InOwner, int64 InAddress, FStringBuilderBase& OutScriptBuilder) const;

    void PushLocation(const FStringBuilderBase& InOutScriptBuilder, const FBlueprintLineInfo& InLineInfo) const;
    void RecordStatement(const FBlueprintNode& InNode, const FStringBuilderBase& InOutScriptBuilder) const;
    void RecordStructural(const FStringBuilderBase& InOutScriptBuilder) const;

    // Marks a statement's address as rendered. False when an earlier edge already rendered that bytecode:
    // the caller then emits a goto instead of drawing the statement a second time.
    bool MarkStatementRendered(const FBlueprintNode& InNode);

    [[nodiscard]] bool HasRenderedStatement(const FBlueprintNode& InNode) const;

    // Render state for the duration of one Visualize call. Both pointers are borrowed and restored by a
    // scope guard on every exit path; ActiveTopLevelBuilder distinguishes a top-level line from one an
    // operand emitted into a builder of its own.
    FBlueprintSourceMap* ActiveSourceMap = nullptr;
    const FStringBuilderBase* ActiveTopLevelBuilder = nullptr;
    // Line records for this render; Build() turns them into the map served with the text. Mutable so the
    // const operand visitors can record the lines they append.
    mutable FBlueprintSourceMapBuilder SourceMapBuilder;
    // Statement addresses already drawn in this render, per function. A branch target that is also another
    // arm's fallthrough reaches the same bytecode twice; the second edge becomes a goto.
    TMap<const UFunction*, TSet<int64>> RenderedStatements;

    static constexpr int32 IndentCount = 16;
    // Inline so the length is visible next to IndentCount and checked by the compiler.
    static constexpr TCHAR Indents[IndentCount + 1] = TEXT("\t\t\t\t\t\t\t\t") TEXT("\t\t\t\t\t\t\t\t");
    static_assert(Indents[IndentCount - 1] == TEXT('\t'), "Indents must hold one tab per level.");
    // Indentation is instance state while the output builder is a parameter, so an operand visitor that
    // starts/ends a scope can shift the ambient level for the line it is spliced into. Keep StartScope
    // and EndScope paired; the level is clamped at IndentCount on indent and at 0 on unindent.
    mutable int32 IndentLevel = 0;
    void Indent() const { IndentLevel = IndentLevel < IndentCount ? IndentLevel + 1 : IndentLevel; }
    void Unindent() const { IndentLevel = IndentLevel > 0 ? IndentLevel - 1 : 0; }

    [[nodiscard]] const TCHAR* GetIndentString() const
    {
        return Indents + FMath::Clamp(IndentCount - IndentLevel, 0, IndentCount);
    }

    FString GetString(const FField* const Value) const;
    FString GetString(const FProperty* const Value) const;
    FString GetString(const UFunction* const Value) const;
    static FString NormaliseString(const FString& Value);

    FString RenderContainerLiteral(const FBlueprintNode& InNode) const;

    // A byte constant assigned to an enum-typed destination renders as the enumerant display name.
    FString RenderEnumAssignment(const FBlueprintNode& InNode, const FBlueprintNode::FNodeVariant& InInput) const;

    void AppendReceiver(const FBlueprintNode& InNode, FStringBuilderBase& OutScriptBuilder) const;

    [[nodiscard]] static const UObject* GetObjectOperand(const FBlueprintNode::FNodeVariant& InVariant);

    [[nodiscard]] FString GetCastTypeName(const UObject* const Value, bool bInterface) const;

    // Per-render caches: a rendered name stays valid for the render that produced it. They return
    // copies, so a caller may hold a result across another lookup.
    mutable TMap<const FField*, FString> FieldCache;
    mutable TMap<const FProperty*, FString> PropertyCache;
    mutable TMap<const UFunction*, FString> FunctionCache;
};