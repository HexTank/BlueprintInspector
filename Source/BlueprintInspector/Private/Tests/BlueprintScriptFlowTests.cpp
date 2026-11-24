
#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "BlueprintScriptFlow.h"

namespace
{
namespace BlueprintScriptFlowTests
{
FBlueprintNode MakeNode(const EExprToken Opcode, const int64 Address)
{
    FBlueprintNode Node;
    Node.Opcode = Opcode;
    Node.ScriptIndex = Address;
    return Node;
}

void Connect(FBlueprintNode& From, FBlueprintNode& To)
{
    From.OutputWires.Emplace_GetRef().Emplace<FBlueprintNode*>(&To);
}
} // namespace BlueprintScriptFlowTests
} // namespace

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScriptFlowMergeTest, "BlueprintInspector.Flow.FindsTheBranchMerge", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintScriptFlowMergeTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintScriptFlowTests;
    namespace Flow = BlueprintScriptFlow;

    FBlueprintNode Branch = MakeNode(EX_JumpIfNot, 0);
    FBlueprintNode FalseArm = MakeNode(EX_IntConst, 10);
    FBlueprintNode TrueArm = MakeNode(EX_IntConst, 20);
    FBlueprintNode Merge = MakeNode(EX_Return, 30);

    Connect(Branch, FalseArm);
    Connect(Branch, TrueArm);
    Connect(FalseArm, Merge);
    Connect(TrueArm, Merge);

    TArray<const FBlueprintNode*> FalseStatements;
    TSet<Flow::FFlowKey> Visited;
    Flow::CollectStatements(Branch.OutputWires[0], FalseStatements, Visited);

    if (!TestEqual(TEXT("the false arm collects its statement then the merge"), FalseStatements.Num(), 2))
    {
        // TestEqual only logs; indexing the arrays here would abort the automation run on a regression.
        return false;
    }
    TestTrue(TEXT("the false arm starts at its own statement"), FalseStatements[0] == &FalseArm);
    TestTrue(TEXT("the second statement is the merge"), FalseStatements[1] == &Merge);

    const FBlueprintNode* FoundMerge = nullptr;
    Flow::FFlowKey MergeKey;
    TestTrue(TEXT("both arms converge"), Flow::FindMerge(Branch, FoundMerge, MergeKey));
    TestTrue(TEXT("the merge is the shared statement"), FoundMerge == &Merge);
    TestEqual(TEXT("the merge key carries its address"), MergeKey.Address, static_cast<int64>(30));

    FBlueprintNode OtherBranch = MakeNode(EX_JumpIfNot, 40);
    FBlueprintNode FalseLeaf = MakeNode(EX_Return, 50);
    FBlueprintNode TrueLeaf = MakeNode(EX_Return, 60);
    Connect(OtherBranch, FalseLeaf);
    Connect(OtherBranch, TrueLeaf);

    FoundMerge = &Merge;
    MergeKey.Address = 123;
    TestFalse(TEXT("disjoint arms have no merge"), Flow::FindMerge(OtherBranch, FoundMerge, MergeKey));
    TestTrue(TEXT("a failed merge search leaves its out-params untouched"), FoundMerge == &Merge);
    TestEqual(TEXT("a failed merge search leaves the key untouched"), MergeKey.Address, static_cast<int64>(123));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScriptFlowLoopTest, "BlueprintInspector.Flow.FindsWhileLoops", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintScriptFlowLoopTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintScriptFlowTests;
    namespace Flow = BlueprintScriptFlow;

    FBlueprintNode Branch = MakeNode(EX_JumpIfNot, 0);
    FBlueprintNode Exit = MakeNode(EX_Return, 10);
    FBlueprintNode Body = MakeNode(EX_IntConst, 20);
    Connect(Branch, Exit);
    Connect(Branch, Body);

    Body.BackEdgeTarget = Branch.ScriptIndex;
    TestTrue(TEXT("a latch back to the branch makes it a loop header"), Flow::IsLoopHeader(Branch));

    // IsLoopHeader is a thin wrapper; assert the payload FindWhileLoop has to fill for the renderer.
    Flow::FLoop WhileLoop;
    TestTrue(TEXT("the branch header forms a while loop"), Flow::FindWhileLoop(Branch, WhileLoop));
    TestTrue(TEXT("the header node is the branch"), WhileLoop.HeaderNode == &Branch);
    TestTrue(TEXT("the condition is the branch itself"), WhileLoop.ConditionNode == &Branch);
    TestTrue(TEXT("the exit is the false arm"), WhileLoop.ExitNode == &Exit);
    TestTrue(TEXT("the latch is the jumping statement"), WhileLoop.Latch.Address == Body.ScriptIndex);
    TestFalse(TEXT("a while loop is not a do/while"), WhileLoop.bDoWhile);
    TestFalse(TEXT("a while loop is not computed"), WhileLoop.bComputedCondition);

    // EX_JumpIfNot is one opcode byte plus a CodeSkipSizeType operand. Pin the operand width as a literal
    // instead of recomputing the offset with the implementation's own expression, so a bytecode layout
    // change fails this test instead of silently following the code under test.
#if SCRIPT_LIMIT_BYTECODE_TO_64KB
    static_assert(sizeof(CodeSkipSizeType) == 2, "the condition-start offset below assumes a 2-byte operand");
    constexpr int64 ConditionOperandWidth = 2;
#else
    static_assert(sizeof(CodeSkipSizeType) == 4, "the condition-start offset below assumes a 4-byte operand");
    constexpr int64 ConditionOperandWidth = 4;
#endif
    Body.BackEdgeTarget = Branch.ScriptIndex + 1 + ConditionOperandWidth;
    TestTrue(TEXT("a latch to the condition makes it a loop header"), Flow::IsLoopHeader(Branch));

    Body.BackEdgeTarget = INDEX_NONE;
    TestFalse(TEXT("a branch without a latch is not a loop"), Flow::IsLoopHeader(Branch));

    FBlueprintNode NotABranch = MakeNode(EX_IntConst, 30);
    TestFalse(TEXT("a non-branch opcode is never a loop header"), Flow::IsLoopHeader(NotABranch));

    FBlueprintNode AfterLoop = MakeNode(EX_Return, 50);
    Body.BackEdgeTarget = Branch.ScriptIndex;
    Connect(Body, AfterLoop);

    TArray<const FBlueprintNode*> BodyStatements;
    TSet<Flow::FFlowKey> Visited;
    if (!Branch.OutputWires.IsValidIndex(1))
    {
        AddError(TEXT("the loop branch has no true arm"));
        return false;
    }
    Flow::CollectStatements(Branch.OutputWires[1], BodyStatements, Visited);
    TestEqual(TEXT("collection stops at the back-edge"), BodyStatements.Num(), 1);
    if (BodyStatements.Num() == 1)
    {
        TestTrue(TEXT("the only body statement is the latch"), BodyStatements[0] == &Body);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScriptFlowComputedWhileTest, "BlueprintInspector.Flow.FindsComputedConditionLoops", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintScriptFlowComputedWhileTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintScriptFlowTests;
    namespace Flow = BlueprintScriptFlow;

    FBlueprintNode Condition = MakeNode(EX_LetBool, 0);
    FBlueprintNode Branch = MakeNode(EX_JumpIfNot, 20);
    FBlueprintNode Exit = MakeNode(EX_Return, 50);
    FBlueprintNode Body = MakeNode(EX_IntConst, 30);
    Connect(Condition, Branch);
    Connect(Branch, Exit);
    Connect(Branch, Body);
    Body.BackEdgeTarget = Condition.ScriptIndex;

    Flow::FLoop Loop;
    TestTrue(TEXT("the condition statement is the loop header"), Flow::FindComputedWhileLoop(Condition, Loop));
    TestTrue(TEXT("the loop is marked as computed"), Loop.bComputedCondition);
    TestFalse(TEXT("the loop is not a do/while"), Loop.bDoWhile);
    TestTrue(TEXT("the header node is the condition statement"), Loop.HeaderNode == &Condition);
    TestTrue(TEXT("the condition node is the branch"), Loop.ConditionNode == &Branch);
    TestTrue(TEXT("the exit is the branch's false arm"), Loop.ExitNode == &Exit);
    TestTrue(TEXT("the latch is the jumping statement"), Loop.Latch.Address == Body.ScriptIndex);

    Flow::FLoop Other;
    TestFalse(TEXT("a branch is never a computed-condition header"), Flow::FindComputedWhileLoop(Branch, Other));

    FBlueprintNode Plain = MakeNode(EX_LetBool, 60);
    FBlueprintNode PlainBranch = MakeNode(EX_JumpIfNot, 70);
    FBlueprintNode PlainExit = MakeNode(EX_Return, 80);
    FBlueprintNode PlainBody = MakeNode(EX_IntConst, 90);
    Connect(Plain, PlainBranch);
    // Both arms must exist: with no wires FindComputedWhileLoop bails out before the latch search, and
    // the case would pass without exercising the "body never jumps back" path.
    Connect(PlainBranch, PlainExit);
    Connect(PlainBranch, PlainBody);
    TestFalse(TEXT("a statement whose body never jumps back is not a loop"), Flow::FindComputedWhileLoop(Plain, Other));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScriptFlowDoWhileTest, "BlueprintInspector.Flow.FindsDoWhileLoops", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintScriptFlowDoWhileTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintScriptFlowTests;
    namespace Flow = BlueprintScriptFlow;

    // A nested branch whose arm jumps back appears before the real tail condition. The direct latch
    // must decide which branch is the condition.
    FBlueprintNode Header = MakeNode(EX_IntConst, 0);
    FBlueprintNode Nested = MakeNode(EX_JumpIfNot, 4);
    FBlueprintNode AfterNested = MakeNode(EX_IntConst, 8);
    FBlueprintNode ContinueArm = MakeNode(EX_IntConst, 6);
    FBlueprintNode Tail = MakeNode(EX_JumpIfNot, 20);
    FBlueprintNode Exit = MakeNode(EX_Return, 40);

    Connect(Header, Nested);
    Connect(Nested, AfterNested);
    Connect(Nested, ContinueArm);
    Connect(AfterNested, Tail);
    Connect(Tail, Exit);
    ContinueArm.BackEdgeTarget = Header.ScriptIndex;
    Tail.BackEdgeTarget = Header.ScriptIndex;
    Tail.BackEdgeWire = 1;

    Flow::FLoop Loop;
    TestTrue(TEXT("the tail condition closes the loop"), Flow::FindDoWhileLoop(Header, Loop));
    TestTrue(TEXT("the direct latch wins over an earlier arm that jumps back"), Loop.ConditionNode == &Tail);
    TestTrue(TEXT("the loop is a do/while"), Loop.bDoWhile);
    TestTrue(TEXT("the exit is the tail's false arm"), Loop.ExitNode == &Exit);
    TestTrue(TEXT("the header node is the body start"), Loop.HeaderNode == &Header);
    TestEqual(TEXT("the latch is the tail condition"), Loop.Latch.Address, Tail.ScriptIndex);

    // The fallback path: no branch carries the back edge itself. The nested branch's true arm reaches
    // the tail through the rest of the body, while the tail's true arm holds the back-edge node. The
    // tail is the last such branch in body order and must be the condition.
    FBlueprintNode LinearHeader = MakeNode(EX_IntConst, 0);
    FBlueprintNode NestedBranch = MakeNode(EX_JumpIfNot, 4);
    FBlueprintNode AfterFallbackNested = MakeNode(EX_IntConst, 8);
    FBlueprintNode NestedArm = MakeNode(EX_IntConst, 6);
    FBlueprintNode TailThrough = MakeNode(EX_IntConst, 12);
    FBlueprintNode FallbackTail = MakeNode(EX_JumpIfNot, 20);
    FBlueprintNode FallbackExit = MakeNode(EX_Return, 40);
    FBlueprintNode BackEdge = MakeNode(EX_IntConst, 24);

    Connect(LinearHeader, NestedBranch);
    Connect(NestedBranch, AfterFallbackNested);
    Connect(NestedBranch, NestedArm);
    Connect(AfterFallbackNested, FallbackTail);
    Connect(NestedArm, TailThrough);
    Connect(TailThrough, FallbackTail);
    Connect(FallbackTail, FallbackExit);
    Connect(FallbackTail, BackEdge);
    BackEdge.BackEdgeTarget = LinearHeader.ScriptIndex;

    Flow::FLoop FallbackLoop;
    TestTrue(TEXT("the arm-latched loop closes"), Flow::FindDoWhileLoop(LinearHeader, FallbackLoop));
    TestTrue(TEXT("the nested branch does not claim the tail condition"), FallbackLoop.ConditionNode == &FallbackTail);
    TestTrue(TEXT("the fallback exit is the tail's false arm"), FallbackLoop.ExitNode == &FallbackExit);
    TestTrue(TEXT("the fallback header node is the linear header"), FallbackLoop.HeaderNode == &LinearHeader);
    TestEqual(TEXT("the fallback latch is the tail condition"), FallbackLoop.Latch.Address, FallbackTail.ScriptIndex);

    // A branch header stays with FindWhileLoop: its branch-latched body has the same shape as a tail
    // condition, so only a linear statement can be told apart as the start of a do/while.
    FBlueprintNode BranchHeader = MakeNode(EX_JumpIfNot, 0);
    FBlueprintNode InnerThen = MakeNode(EX_IntConst, 4);
    FBlueprintNode BranchTail = MakeNode(EX_JumpIfNot, 20);
    FBlueprintNode BranchExit = MakeNode(EX_Return, 40);

    Connect(BranchHeader, BranchExit);
    Connect(BranchHeader, InnerThen);
    Connect(InnerThen, BranchTail);
    BranchTail.BackEdgeTarget = BranchHeader.ScriptIndex;
    BranchTail.BackEdgeWire = 1;

    Flow::FLoop BranchLoop;
    TestFalse(TEXT("a branch header stays with FindWhileLoop"), Flow::FindDoWhileLoop(BranchHeader, BranchLoop));

    // The branch header is recovered as a while loop, and its false arm really exits the loop.
    TestTrue(TEXT("the branch header is still recovered as a while loop"), Flow::FindWhileLoop(BranchHeader, BranchLoop));
    TestTrue(TEXT("the while loop exits through the false arm"), BranchLoop.ExitNode == &BranchExit);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintScriptFlowJoinTest, "BlueprintInspector.Flow.RejectsAMergeAnArmCanSkip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintScriptFlowJoinTest::RunTest(const FString& Parameters)
{
    using namespace BlueprintScriptFlowTests;
    namespace Flow = BlueprintScriptFlow;

    FBlueprintNode Branch = MakeNode(EX_JumpIfNot, 0);
    FBlueprintNode Shared = MakeNode(EX_IntConst, 10);
    FBlueprintNode SharedTail = MakeNode(EX_IntConst, 20);
    FBlueprintNode Probe = MakeNode(EX_IntConst, 40);
    FBlueprintNode Inner = MakeNode(EX_JumpIfNot, 50);
    FBlueprintNode Join = MakeNode(EX_IntConst, 60);
    FBlueprintNode Exit = MakeNode(EX_Return, 70);

    Connect(Branch, Shared);
    Connect(Branch, Probe);
    Connect(Shared, SharedTail);
    Connect(SharedTail, Join);
    Connect(Probe, Inner);
    Connect(Inner, Shared);
    Connect(Inner, Join);
    Connect(Join, Exit);

    const FBlueprintNode* Merge = nullptr;
    Flow::FFlowKey MergeKey;
    TestTrue(TEXT("the statement after both arms is the join"), Flow::FindMerge(Branch, Merge, MergeKey));
    TestTrue(TEXT("the shared block both arms jump into is not the join"), Merge == &Join);
    TestEqual(TEXT("the join key carries its address"), MergeKey.Address, static_cast<int64>(60));

    FBlueprintNode EndingBranch = MakeNode(EX_JumpIfNot, 100);
    FBlueprintNode EndingShared = MakeNode(EX_IntConst, 110);
    FBlueprintNode EndingProbe = MakeNode(EX_IntConst, 140);
    FBlueprintNode EndingInner = MakeNode(EX_JumpIfNot, 150);
    FBlueprintNode EndingJoin = MakeNode(EX_IntConst, 160);
    FBlueprintNode EndingExit = MakeNode(EX_Return, 170);

    Connect(EndingBranch, EndingShared);
    Connect(EndingBranch, EndingProbe);
    Connect(EndingProbe, EndingInner);
    Connect(EndingInner, EndingShared);
    Connect(EndingInner, EndingJoin);
    Connect(EndingShared, EndingExit);

    Merge = nullptr;
    MergeKey.Address = 123;
    TestFalse(TEXT("an arm that ends before the shared block leaves no join"), Flow::FindMerge(EndingBranch, Merge, MergeKey));
    TestTrue(TEXT("a rejected join search leaves its node untouched"), Merge == nullptr);
    TestEqual(TEXT("a rejected join search leaves the key untouched"), MergeKey.Address, static_cast<int64>(123));
    return true;
}

#endif
