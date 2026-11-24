#include "BlueprintScriptFlow.h"

namespace BlueprintScriptFlow
{
namespace
{
bool ReachesExitBefore(const FBlueprintNode::FWireVariant& InWire, const FFlowKey& InMerge)
{
    if (!InWire.IsType<FBlueprintNode*>())
    {
        return false;
    }

    TSet<const FBlueprintNode*> Visited;
    TArray<const FBlueprintNode*> Pending;
    Pending.Add(InWire.Get<FBlueprintNode*>());

    while (Pending.Num() > 0)
    {
        const FBlueprintNode* const Node = Pending.Pop(BI_POP_NO_SHRINK);
        if (Node == nullptr || Visited.Contains(Node) || SameKey(*Node, InMerge))
        {
            continue;
        }
        Visited.Add(Node);

        bool bContinues = Node->BackEdgeTarget != INDEX_NONE;
        for (const FBlueprintNode::FWireVariant& Wire : Node->OutputWires)
        {
            const FBlueprintNode* const* const Child = Wire.TryGet<FBlueprintNode*>();
            if (Child != nullptr && *Child != nullptr)
            {
                bContinues = true;
                Pending.Add(*Child);
            }
        }

        if (!bContinues)
        {
            return true;
        }
    }

    return false;
}

// The fallthrough landing of an EX_JumpIfNot: the opcode byte plus one CodeSkipSizeType operand. If
// the compiler's layout changes, this offset (and every latch that compares against it) is wrong, so it
// is kept in one helper rather than inlined at the call sites.
int64 GetFallthroughLanding(const FBlueprintNode& InBranch)
{
    return InBranch.ScriptIndex + 1 + static_cast<int64>(sizeof(CodeSkipSizeType));
}
} // namespace

FFlowKey GetKey(const FBlueprintNode& InNode)
{
    return FFlowKey{InNode.FunctionKey, InNode.ScriptIndex};
}

bool SameKey(const FBlueprintNode& InNode, const FFlowKey& InKey)
{
    return InNode.FunctionKey == InKey.Function && InNode.ScriptIndex == InKey.Address;
}

void CollectStatements(const FBlueprintNode::FWireVariant& InWire, TArray<const FBlueprintNode*>& OutNodes, TSet<FFlowKey>& Visited)
{
    if (!InWire.IsType<FBlueprintNode*>())
    {
        return;
    }

    const FBlueprintNode* const Node = InWire.Get<FBlueprintNode*>();
    if (Node == nullptr)
    {
        return;
    }

    CollectStatements(*Node, OutNodes, Visited);
}

void CollectStatements(const FBlueprintNode& InNode, TArray<const FBlueprintNode*>& OutNodes, TSet<FFlowKey>& Visited)
{
    ensureAlwaysMsgf(Visited.Num() == 0, TEXT("CollectStatements requires an empty Visited: the key identity is module-wide, so a reused set drops statements"));
    Visited.Reset();

    // An explicit stack, not recursion: the recursive walk pushed one C++ frame per statement, so the
    // stack depth was the size of the function's graph. Pushing children in reverse keeps the visit order
    // identical to the recursive version (a node, then its output wires in order).
    TArray<const FBlueprintNode*> Pending;
    Pending.Add(&InNode);

    while (Pending.Num() > 0)
    {
        const FBlueprintNode* const Node = Pending.Last();
        Pending.Pop();

        if (Node == nullptr)
        {
            continue;
        }

        const FFlowKey Key = GetKey(*Node);
        if (Visited.Contains(Key))
        {
            continue;
        }
        Visited.Add(Key);
        OutNodes.Add(Node);

        // A back-edge closes a loop. A linear latch ends the walk; a branch whose arm jumped back continues
        // through its other arm, which still belongs to the body.
        if (Node->BackEdgeTarget != INDEX_NONE && (Node->Opcode != EX_JumpIfNot || Node->BackEdgeWire == INDEX_NONE))
        {
            continue;
        }

        for (int32 Index = Node->OutputWires.Num() - 1; Index >= 0; --Index)
        {
            const FBlueprintNode::FWireVariant& Child = Node->OutputWires[Index];
            if (Child.IsType<FBlueprintNode*>() && Child.Get<FBlueprintNode*>() != nullptr)
            {
                Pending.Add(Child.Get<FBlueprintNode*>());
            }
        }
    }
}

bool FindMerge(const FBlueprintNode& InBranch, const FBlueprintNode*& OutMerge, FFlowKey& OutKey)
{
    if (InBranch.OutputWires.Num() < 2)
    {
        return false;
    }

    TArray<const FBlueprintNode*> FalseArm;
    TSet<FFlowKey> FalseVisited;
    CollectStatements(InBranch.OutputWires[0], FalseArm, FalseVisited);

    TArray<const FBlueprintNode*> TrueArm;
    TSet<FFlowKey> TrueVisited;
    CollectStatements(InBranch.OutputWires[1], TrueArm, TrueVisited);

    TSet<FFlowKey> TrueKeys;
    TrueKeys.Reserve(TrueArm.Num());
    for (const FBlueprintNode* const Node : TrueArm)
    {
        TrueKeys.Add(GetKey(*Node));
    }

    for (const FBlueprintNode* const Node : FalseArm)
    {
        const FFlowKey Key = GetKey(*Node);
        if (!TrueKeys.Contains(Key) || ReachesExitBefore(InBranch.OutputWires[0], Key) || ReachesExitBefore(InBranch.OutputWires[1], Key))
        {
            continue;
        }
        OutMerge = Node;
        OutKey = Key;
        return true;
    }

    return false;
}

bool IsLoopHeader(const FBlueprintNode& InBranch)
{
    FLoop Loop;
    return FindWhileLoop(InBranch, Loop);
}

bool FindComputedWhileLoop(const FBlueprintNode& InCondition, FLoop& OutLoop)
{
    if (InCondition.Opcode == EX_JumpIfNot || InCondition.Opcode == EX_Max || InCondition.Opcode == EX_PushExecutionFlow || InCondition.Opcode == EX_PopExecutionFlow)
    {
        return false;
    }

    // The condition statement stores the value the branch that follows reads. Only the single-statement
    // form is recovered; a multi-term expression is still claimed by the do/while analysis.
    if (InCondition.OutputWires.Num() < 1 || !InCondition.OutputWires[0].IsType<FBlueprintNode*>())
    {
        return false;
    }

    const FBlueprintNode* const Branch = InCondition.OutputWires[0].Get<FBlueprintNode*>();
    if (Branch == nullptr || Branch->Opcode != EX_JumpIfNot || Branch->OutputWires.Num() < 2)
    {
        return false;
    }

    TArray<const FBlueprintNode*> Body;
    TSet<FFlowKey> Visited;
    CollectStatements(Branch->OutputWires[1], Body, Visited);

    const FBlueprintNode* Latch = nullptr;
    for (const FBlueprintNode* const Node : Body)
    {
        if (Node->BackEdgeTarget == InCondition.ScriptIndex)
        {
            // Keep the last match: a `continue` also jumps back to the condition statement, but the
            // loop's latch is the terminal tail test.
            Latch = Node;
        }
    }

    if (Latch == nullptr)
    {
        return false;
    }

    OutLoop = FLoop{};
    OutLoop.Header = GetKey(InCondition);
    OutLoop.HeaderNode = &InCondition;
    OutLoop.Condition = GetKey(*Branch);
    OutLoop.ConditionNode = Branch;
    OutLoop.Latch = GetKey(*Latch);
    OutLoop.bDoWhile = false;
    OutLoop.bComputedCondition = true;

    if (Branch->OutputWires[0].IsType<FBlueprintNode*>())
    {
        if (const FBlueprintNode* const Exit = Branch->OutputWires[0].Get<FBlueprintNode*>())
        {
            OutLoop.Exit = GetKey(*Exit);
            OutLoop.ExitNode = Exit;
        }
    }

    return OutLoop.ExitNode != nullptr;
}

bool FindWhileLoop(const FBlueprintNode& InBranch, FLoop& OutLoop)
{
    if (InBranch.Opcode != EX_JumpIfNot || InBranch.OutputWires.Num() < 2)
    {
        return false;
    }

    TArray<const FBlueprintNode*> Body;
    TSet<FFlowKey> Visited;
    CollectStatements(InBranch.OutputWires[1], Body, Visited);

    const int64 FallthroughLanding = GetFallthroughLanding(InBranch);
    const FBlueprintNode* Latch = nullptr;
    for (const FBlueprintNode* const Node : Body)
    {
        if (Node->BackEdgeTarget == InBranch.ScriptIndex || Node->BackEdgeTarget == FallthroughLanding || Node->BackEdgeLanding == FallthroughLanding)
        {
            Latch = Node;
        }
    }

    if (Latch == nullptr)
    {
        return false;
    }

    OutLoop = FLoop{};
    OutLoop.Header = GetKey(InBranch);
    OutLoop.HeaderNode = &InBranch;
    OutLoop.Condition = OutLoop.Header;
    OutLoop.ConditionNode = &InBranch;
    OutLoop.Latch = GetKey(*Latch);
    OutLoop.bDoWhile = false;

    if (InBranch.OutputWires[0].IsType<FBlueprintNode*>())
    {
        if (const FBlueprintNode* const Exit = InBranch.OutputWires[0].Get<FBlueprintNode*>())
        {
            OutLoop.Exit = GetKey(*Exit);
            OutLoop.ExitNode = Exit;
        }
    }

    return OutLoop.ExitNode != nullptr;
}

bool FindDoWhileLoop(const FBlueprintNode& InHeader, FLoop& OutLoop)
{
    // A branch header stays with FindWhileLoop: a while loop can latch through a branch arm (a
    // `continue`), which has the same shape as a tail condition. Only a linear statement can be
    // distinguished as the body start of a do/while.
    if (InHeader.Opcode == EX_JumpIfNot || InHeader.Opcode == EX_Max || InHeader.Opcode == EX_PushExecutionFlow || InHeader.Opcode == EX_PopExecutionFlow)
    {
        return false;
    }

    TArray<const FBlueprintNode*> Body;
    TSet<FFlowKey> Visited;
    CollectStatements(InHeader, Body, Visited);

    // The condition is the branch whose own jump is the latch. A branch that only reaches the header
    // through one arm is consulted only when no direct latch exists, so a nested branch that happens to
    // jump back cannot claim the loop. If several branches latch, the last in body order is the tail.
    const FBlueprintNode* Condition = nullptr;
    for (const FBlueprintNode* const Node : Body)
    {
        if (Node != &InHeader && Node->Opcode == EX_JumpIfNot && Node->BackEdgeTarget == InHeader.ScriptIndex)
        {
            Condition = Node;
        }
    }

    if (Condition == nullptr)
    {
        // Memoise "this subtree contains a back edge to the header" over the body. A false reached only
        // by stepping through a frame that is still being evaluated holds for that path alone, because
        // the frame may reach the header itself, so it is not cached; caching it would answer for a
        // later, unrelated path.
        TMap<const FBlueprintNode*, bool> ReachesHeader;
        TSet<const FBlueprintNode*> InProgress;
        constexpr int32 MaxFlowDepth = 512;
        TFunction<bool(const FBlueprintNode*, bool&, int32)> CanReachBackEdge;
        CanReachBackEdge = [&](const FBlueprintNode* const Node, bool& bCycleDependent, const int32 Depth) -> bool {
            bCycleDependent = false;
            if (Node == nullptr)
            {
                return false;
            }
            if (Depth > MaxFlowDepth)
            {
                bCycleDependent = true;
                return false;
            }
            if (const bool* const Cached = ReachesHeader.Find(Node))
            {
                return *Cached;
            }
            if (InProgress.Contains(Node))
            {
                // A cycle that has not reached the header yet; the outer frames decide the result.
                bCycleDependent = true;
                return false;
            }

            InProgress.Add(Node);
            bool bReaches = Node->BackEdgeTarget == InHeader.ScriptIndex;
            if (!bReaches)
            {
                // A node whose back edge goes elsewhere only continues through its other branch arm;
                // any other back edge ends the search.
                const bool bContinue = Node->BackEdgeTarget == INDEX_NONE || (Node->Opcode == EX_JumpIfNot && Node->BackEdgeWire != INDEX_NONE);
                if (bContinue)
                {
                    for (const FBlueprintNode::FWireVariant& Wire : Node->OutputWires)
                    {
                        if (!Wire.IsType<FBlueprintNode*>())
                        {
                            continue;
                        }
                        bool bChildCycleDependent = false;
                        const bool bChildReaches = CanReachBackEdge(Wire.Get<FBlueprintNode*>(), bChildCycleDependent, Depth + 1);
                        bCycleDependent |= bChildCycleDependent;
                        if (bChildReaches)
                        {
                            bReaches = true;
                            break;
                        }
                    }
                }
            }
            InProgress.Remove(Node);

            // A true path is true however it was found; only a cycle-independent false is safe to reuse.
            if (bReaches || !bCycleDependent)
            {
                ReachesHeader.Add(Node, bReaches);
            }
            return bReaches;
        };

        const auto ArmReaches = [&CanReachBackEdge](const FBlueprintNode& Branch, const int32 ArmIndex) {
            if (!Branch.OutputWires.IsValidIndex(ArmIndex) || !Branch.OutputWires[ArmIndex].IsType<FBlueprintNode*>())
            {
                return false;
            }
            bool bCycleDependent = false;
            return CanReachBackEdge(Branch.OutputWires[ArmIndex].Get<FBlueprintNode*>(), bCycleDependent, 0);
        };

        // The true arm is the loop-back arm. Body is a DFS pre-order from the header, so the tail
        // condition is the last branch whose true arm reaches the back edge; an earlier nested branch
        // that only reaches the header through the rest of the body must not claim it.
        for (const FBlueprintNode* const Node : Body)
        {
            if (Node != &InHeader && Node->Opcode == EX_JumpIfNot && ArmReaches(*Node, 1))
            {
                Condition = Node;
            }
        }
    }

    if (Condition == nullptr || Condition->OutputWires.Num() < 1)
    {
        return false;
    }

    OutLoop = FLoop{};
    OutLoop.Header = GetKey(InHeader);
    OutLoop.HeaderNode = &InHeader;
    OutLoop.Condition = GetKey(*Condition);
    OutLoop.ConditionNode = Condition;
    OutLoop.Latch = OutLoop.Condition;
    OutLoop.bDoWhile = true;

    if (Condition->OutputWires[0].IsType<FBlueprintNode*>())
    {
        if (const FBlueprintNode* const Exit = Condition->OutputWires[0].Get<FBlueprintNode*>())
        {
            OutLoop.Exit = GetKey(*Exit);
            OutLoop.ExitNode = Exit;
        }
    }

    return OutLoop.ExitNode != nullptr;
}
} // namespace BlueprintScriptFlow
