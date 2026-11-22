#pragma once

#include "CoreMinimal.h"
#include "BlueprintInspectorPort.h"
#include "BlueprintOpcodeLogger.h"
#include "BlueprintScriptArchiveReader.h"
#include "Misc/TVariant.h"
#include "UObject/Class.h"
#include "UObject/ObjectKey.h"

struct FContextProperty
{
    const FProperty* Context = nullptr;
    const FProperty* Property = nullptr;
};

// One pending flow edge. Address is where the walk resumes, with debug opcodes skipped. JumpOpcode and
// Landing are the raw pair the VM itself uses, so a break/continue line can name the exact dispatch it
// waits for: the opcode the detour sees as Code - 1, and the offset Code holds once that handler has run.
struct FProcessWireData
{
    TWeakObjectPtr<const UFunction> Function;
    int64 Address = 0;
    int64 JumpOpcode = INDEX_NONE;
    int64 Landing = INDEX_NONE;
};

enum class EBlueprintContainerKind : uint8
{
    None,
    Array,
    Set,
    Map,
};

// The type information a container literal carries. The nodes keep their elements in InputNodes; this
// says what those elements are, and for a map, that they are key/value pairs rather than a flat list.
struct FBlueprintContainerLiteral
{
    EBlueprintContainerKind Kind = EBlueprintContainerKind::None;
    const FProperty* ElementType = nullptr; // Array/Set element, or Map key
    const FProperty* ValueType = nullptr;   // Map value
    int32 Count = 0;                        // Element count, or Map pair count
};

// A delegate literal from EX_InstanceDelegate. The opcode carries only the bound function name; keeping
// it as its own type lets the renderer show a delegate literal rather than an untyped quoted string.
struct FDelegateLiteral
{
    FName FunctionName;
};

struct FBlueprintNode
{
    FBlueprintNode() = default;
    FBlueprintNode(FBlueprintNode&&) = default;
    FBlueprintNode& operator=(FBlueprintNode&&) = default;
    // A copy would duplicate the raw wire pointers, so two nodes would claim the same wires.
    FBlueprintNode(const FBlueprintNode&) = delete;
    FBlueprintNode& operator=(const FBlueprintNode&) = delete;

    explicit FBlueprintNode(const TWeakObjectPtr<const UFunction>& InFunctionOwner, const FName& InNodeName, const EExprToken InOpcode) : FunctionOwner(InFunctionOwner), FunctionKey(InFunctionOwner.Get()), Name(InNodeName), Opcode(InOpcode) {}

    explicit FBlueprintNode(const TWeakObjectPtr<const UFunction>& InFunctionOwner, FName&& InNodeName, const EExprToken InOpcode) : FunctionOwner(InFunctionOwner), FunctionKey(InFunctionOwner.Get()), Name(MoveTemp(InNodeName)), Opcode(InOpcode) {}

    TWeakObjectPtr<const UFunction> FunctionOwner;
    FObjectKey FunctionKey;
    FName Name;
    bool bCalleeHasWorldContext = false;
    int64 ScriptIndex = INDEX_NONE;
    EExprToken Opcode = EX_Max;

    // Set on the node whose flow edge jumped to an address the walk had already taken. The edge is not in
    // OutputWires (the walk stops there), so the structurer uses this to recognise a loop latch: a node with
    // BackEdgeTarget == the branch's address closes a while loop at that branch. BackEdgeWire is the output
    // arm that jumped (0 = false/fallthrough arm of a branch, 1 = true arm); for a linear node it is the
    // arm the node would have grown, which is not an arm at all.
    int64 BackEdgeTarget = INDEX_NONE;
    int32 BackEdgeWire = INDEX_NONE;

    // The dispatch that closed the edge: the jump opcode's offset and the offset the VM holds once that
    // handler has run. BackEdgeTarget is a node address, and an editor build can park a debug opcode before
    // it, so it is not the landing: a break/continue stop matches on these two instead.
    int64 BackEdgeJumpOpcode = INDEX_NONE;
    int64 BackEdgeLanding = INDEX_NONE;

    // The jump edge that first arrived at this node's own opcode. A `break` that is a bare jump to the loop
    // exit owns no node of its own: the line it renders belongs to the exit node, so the dispatch pair has
    // to travel with the arrival.
    int64 ArrivalJumpOpcode = INDEX_NONE;
    int64 ArrivalLanding = INDEX_NONE;

    using FWireVariant = TVariant<FBlueprintNode*, int64>;
    TArray<FBlueprintNode*> InputWires;
    TArray<FWireVariant> OutputWires;
    TArray<FProcessWireData> WiresToProcess;

#if BI_ENGINE_AT_LEAST_5
    using FNodeVariant = TVariant<FBlueprintNode*, const UObject*, FContextProperty, EExprToken, FName, FDelegateLiteral, int64, uint64, double, const FVector*, const FRotator*, const BI_FVector3f*, const FTransform*>;
#else
    using FNodeVariant = TVariant<FBlueprintNode*, const UObject*, FContextProperty, EExprToken, FName, FDelegateLiteral, int64, uint64, double, const FVector*, const FRotator*, const FTransform*>;
#endif
    TArray<FNodeVariant> InputNodes;

    TOptional<FNodeVariant> Receiver;

    FContextProperty ReturnNode;
    FBlueprintContainerLiteral ContainerLiteral;
};

class FBlueprintScriptDisassembler
{
public:
    FBlueprintScriptDisassembler();
    FBlueprintScriptDisassembler(const FBlueprintScriptDisassembler&) = delete;
    FBlueprintScriptDisassembler& operator=(const FBlueprintScriptDisassembler&) = delete;
    FBlueprintScriptDisassembler(FBlueprintScriptDisassembler&&) = delete;
    FBlueprintScriptDisassembler& operator=(FBlueprintScriptDisassembler&&) = delete;

    enum class EDirection : uint8
    {
        None,
        Input,
        Output
    };

    // Fills the graph for InFunction. False when the function is invalid, the start offset is outside
    // its script, or the reader hit a malformed operand; the graph may then be empty or partial, so a
    // caller that publishes it should treat the result as best-effort rather than a complete capture.
    bool FunctionToNodes(const TWeakObjectPtr<const UFunction>& InFunction, int64 InStartAddress = 0);
    FBlueprintNode& GetEntryPointNode() { return EntryPointNode; }
    const FBlueprintNode& GetEntryPointNode() const { return EntryPointNode; }

private:
    bool SeekToOffset(const TWeakObjectPtr<const UFunction>& InFunction, const int64 InAddress = 0);

    EExprToken DecodeOpcode(const EDirection InDirection);
    EExprToken DecodeOpcodeUntil(const EExprToken InOpcode, const EDirection InDirection);
    EExprToken ProcessOpcode(const EExprToken InOpcode, const EDirection InDirection);
    EExprToken FinishFlow(const EExprToken InOpcode);

    template <typename T, typename U = T, bool bLog = true>
    void EmitOperand();

    // As EmitOperand, for the geometry constants. The script stores those inline, so the operand is a
    // pointer into the bytecode and reading the value is only there to advance the cursor past it.
    template <typename T>
    void EmitScriptPointerOperand();

    const FVector* StoreConstant(const FVector& Value);
    const FRotator* StoreConstant(const FRotator& Value);
#if BI_ENGINE_AT_LEAST_5
    const BI_FVector3f* StoreConstant(const BI_FVector3f& Value);
#endif

    [[nodiscard]] FBlueprintNode* DecodeFunctionData(const EExprToken InOpcode, const EDirection InDirection, const UFunction* const InFunction);
    FBlueprintNode* PushNewNode(const FName& InName, const EExprToken InOpcode);
    [[nodiscard]] int64 SanitiseAddress(const int64 InAddress);

    FBlueprintScriptArchiveReader ScriptReader;

    FBlueprintNode EntryPointNode;
    TArray<FBlueprintNode*> Stack;

    // Fixed node pool. Stack, the wire lists and NodeIterator hold raw pointers into Nodes, so the array
    // must never reallocate during a run: allocate all slots up front and treat the capacity as
    // immutable. Nodes beyond the pool come from OverflowNodes.
    static constexpr int32 NodePoolCapacity = 1024;
    TArray<FBlueprintNode> Nodes;
    FBlueprintNode* NodeIterator = nullptr;

    // Nodes created after the fixed pool is exhausted. Heap storage keeps every address stable for the
    // life of the run; reusing one member made earlier wires alias a node that later carried another
    // opcode, and a self-reference defeated the callers' LiftToOperand/pop pattern.
    TArray<TUniquePtr<FBlueprintNode>> OverflowNodes;

    // Scratch parents for the object expression of an EX_Context, one per nesting level. Nodes created
    // while a scratch node is on top of Stack keep an InputWire to it, so it has to live as long as the
    // graph; a single shared scratch node was reset by a nested context and destroyed the outer
    // receiver expression.
    TArray<TUniquePtr<FBlueprintNode>> ContextScratchNodes;
    int32 ContextDepth = 0;
    bool bNodePoolExhaustedLogged = false;
    int32 DecodeDepth = 0;
    static constexpr int32 MaxDecodeDepth = 512;
    TWeakObjectPtr<const UFunction> CurrentFunction;

    // Stable storage for inline transform constants. The visualizer holds pointers into this pool, so it
    // must not move while the node graph is rendered. Fixed capacity: once it is exhausted the operand
    // is still consumed but not linked, and the run is marked incomplete.
    static constexpr int32 TransformPoolCapacity = 256;
    TArray<FTransform> TransformPool;
    int32 TransformPoolCount = 0;
    bool bTransformPoolExhaustedLogged = false;

    // The same stable storage for the other inline geometry constants: the visualizer dereferences the
    // pointer after the walk, and a pointer into the bytecode is unaligned.
    static constexpr int32 ConstantPoolCapacity = 256;
    TArray<FVector> VectorPool;
    int32 VectorPoolCount = 0;
    bool bVectorPoolExhaustedLogged = false;
    TArray<FRotator> RotatorPool;
    int32 RotatorPoolCount = 0;
    bool bRotatorPoolExhaustedLogged = false;
#if BI_ENGINE_AT_LEAST_5
    TArray<BI_FVector3f> Vector3fPool;
    int32 Vector3fPoolCount = 0;
    bool bVector3fPoolExhaustedLogged = false;
#endif

    FBlueprintOpcodeLogger OpcodeLogger;
    int64 CurrentOpcodeScriptIndex = 0;
    TWeakObjectPtr<const UClass> FunctionOwnerClass;

    // The context's object expression, parked until the member it belongs to creates a node.
    // Parked receivers, innermost last: a context parks the object expression that the first node its
    // member decode creates displays as "Object.Member". One slot per nesting level, so a context inside
    // another context's member expression cannot overwrite the enclosing one.
    TArray<TOptional<FBlueprintNode::FNodeVariant>> PendingReceivers;

    // A jump edge that the walk followed to an address it had not taken yet. The node created at that
    // address adopts the pair, so a break rendered on the loop exit can still name its own dispatch.
    int64 PendingArrivalAddress = INDEX_NONE;
    int64 PendingArrivalJumpOpcode = INDEX_NONE;
    int64 PendingArrivalLanding = INDEX_NONE;
};