#pragma once

#include "CoreMinimal.h"
#include "BlueprintScriptDisassembler.h"
#include "UObject/ObjectKey.h"

/**
 * Graph-only control-flow analysis for the pseudo-source renderer. These functions read the node graph the
 * disassembler built and answer structural questions; they never render text, build a source map, or touch
 * the visualizer's state. That keeps the algorithms testable on hand-built graphs.
 */
namespace BlueprintScriptFlow
{
/**
 * A statement's identity across the duplicated nodes the walk creates for a convergent edge: the owner
 * function plus the bytecode offset. Two nodes with the same key are the same bytecode.
 *
 * Function is an FObjectKey so the identity survives a collected function: a raw pointer would let a
 * later function allocated at the same address inherit the old one's keys, and every collected function
 * would otherwise degrade to the same (nullptr, Address) identity. Keys are still only meaningful for
 * the disassembly pass that produced them; do not retain them across passes.
 */
struct FFlowKey
{
    FObjectKey Function;
    int64 Address = INDEX_NONE;

    bool operator==(const FFlowKey& Other) const { return Function == Other.Function && Address == Other.Address; }

    friend uint32 GetTypeHash(const FFlowKey& Key)
    {
        // The FObjectKey overload is a friend of that type, so it is only found by argument-dependent
        // lookup: an unqualified call finds it. The numeric overload stays qualified, because an
        // unqualified call there would only see the overloads declared in this header on some compilers
        // (notably UE 4.25's MSVC line).
        return HashCombine(GetTypeHash(Key.Function), ::GetTypeHash(Key.Address));
    }
};

FFlowKey GetKey(const FBlueprintNode& InNode);
bool SameKey(const FBlueprintNode& InNode, const FFlowKey& InKey);

/**
 * Every statement reachable from a wire, in render order (a node, then its output wires in order). A
 * linear node that jumped back ends its path; a branch whose arm jumped back skips that arm but still
 * visits the branch's other arms.
 *
 * Visited is in/out: it must be empty when starting a fresh traversal (the key identity is module-wide,
 * so a reused set silently drops statements) and is seeded by this call. OutNodes is appended to and
 * never cleared by this function.
 */
void CollectStatements(const FBlueprintNode::FWireVariant& InWire, TArray<const FBlueprintNode*>& OutNodes, TSet<FFlowKey>& Visited);

void CollectStatements(const FBlueprintNode& InNode, TArray<const FBlueprintNode*>& OutNodes, TSet<FFlowKey>& Visited);

/** Starts a fresh traversal with an empty visited set. */
inline void CollectStatements(const FBlueprintNode::FWireVariant& InWire, TArray<const FBlueprintNode*>& OutNodes)
{
    TSet<FFlowKey> Visited;
    CollectStatements(InWire, OutNodes, Visited);
}

/**
 * The join of a branch: the first statement both arms reach where neither arm can end its flow before
 * reaching it. Render order decides, so the first statement the false arm reaches that the true arm also
 * reaches and that is on every path from the branch is the merge. A block both arms can jump into is not a
 * join when an arm can also leave without it.
 *
 * Requires the branch wire layout the disassembler produces: OutputWires[0] is the false arm and
 * OutputWires[1] is the true arm. Returns false for a node with fewer than two wires or when a wire holds
 * an int64 jump target instead of a node. OutMerge/OutKey are written only on success; on false they are
 * left untouched, so a caller must not read them from a previous call.
 */
bool FindMerge(const FBlueprintNode& InBranch, const FBlueprintNode*& OutMerge, FFlowKey& OutKey);

/**
 * True only when the branch is the header of a self-contained while loop whose exit arm resolves to a
 * node: FindWhileLoop accepts a latch to the branch or to the branch's fall-through address, and fails
 * when the loop exits the function. False therefore does not mean "not a loop"; a computed-condition
 * while loop (latch to the statement before the branch) is found by FindComputedWhileLoop instead.
 */
bool IsLoopHeader(const FBlueprintNode& InBranch);

/**
 * A loop region recovered from the node graph. `ConditionNode` is the branch whose condition controls the
 * loop; `Exit` is the first statement after it. For a while loop the header and condition are the same
 * branch; for a do/while the header is the body's first statement and the condition is the tail branch.
 *
 * Invariants: a default-constructed FLoop is empty (all keys INDEX_NONE, all node pointers null). A
 * successful Find* fills Header and Condition, sets HeaderNode/ConditionNode to the same nodes, and
 * sets Exit/ExitNode when the exit arm resolves to a node. All three finders return false when the
 * exit arm is unresolvable (the loop leaves the function, or the arm is not a node); on false OutLoop
 * may be untouched or partially filled, so do not use it. For bDoWhile the header is a linear
 * statement; for bComputedCondition the header is the condition statement and Condition is the
 * branch that consumes it, so Header != Condition. Latch is the back edge the detector matched; when
 * only an arm jump identified the loop, FindDoWhileLoop leaves it equal to Condition. There is no
 * separate validity helper: test the keys you need (or the bool return) rather than assuming a fresh
 * struct is filled.
 */
struct FLoop
{
    FFlowKey Header;
    FFlowKey Condition;
    FFlowKey Exit;
    FFlowKey Latch;
    const FBlueprintNode* HeaderNode = nullptr;
    const FBlueprintNode* ConditionNode = nullptr;
    const FBlueprintNode* ExitNode = nullptr;
    bool bDoWhile = false;
    // The condition is an expression statement before the branch (the compiler stores its result in a
    // temp). Header is that statement; Condition is the branch that consumes it.
    bool bComputedCondition = false;
};

bool FindWhileLoop(const FBlueprintNode& InBranch, FLoop& OutLoop);

/**
 * A while loop whose condition is computed: the statement stores the comparison result and the branch
 * that follows reads it, and the latch jumps back to that statement rather than to the branch. Header
 * is the condition statement, Condition is the branch, and the body is the branch's fall-through.
 */
bool FindComputedWhileLoop(const FBlueprintNode& InCondition, FLoop& OutLoop);

/**
 * A statement followed by a branch whose arm jumps back to it is a do/while. Only a linear statement is
 * accepted as the header (a branch header belongs to FindWhileLoop). The body walk covers the whole
 * downstream flow per call; the per-candidate arm search is memoised over that body, so it does not
 * re-walk the same subgraph for every branch.
 */
bool FindDoWhileLoop(const FBlueprintNode& InHeader, FLoop& OutLoop);
} // namespace BlueprintScriptFlow
