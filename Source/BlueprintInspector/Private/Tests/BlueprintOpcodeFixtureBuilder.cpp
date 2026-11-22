#include "BlueprintOpcodeFixtureBuilder.h"

#if WITH_EDITOR

#include "BlueprintInspector.h"
#include "BlueprintInspectorPort.h"

#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNode.h"
#include "EdGraph/EdGraphPin.h"
#include "EdGraphSchema_K2.h"
#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"
#include "K2Node_CallFunction.h"
#include "K2Node_CreateDelegate.h"
#include "K2Node_CustomEvent.h"
#include "K2Node_FunctionEntry.h"
#include "K2Node_FunctionResult.h"
#include "K2Node_FunctionTerminator.h"
#include "K2Node_IfThenElse.h"
#include "K2Node_Self.h"
#include "K2Node_SwitchInteger.h"
#include "K2Node_VariableGet.h"
#include "K2Node_VariableSet.h"
#include "Kismet/KismetMathLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/EnumEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/PackageName.h"
#include "UObject/SavePackage.h"
#include "UObject/UnrealType.h"

#include "OpcodeProbeHost.h"

namespace
{
UEdGraphPin* Pin(UEdGraphNode* const Node, const TCHAR* const Name, const EEdGraphPinDirection Direction = EGPD_MAX)
{
    return Node != nullptr ? Node->FindPin(Name, Direction) : nullptr;
}

template <typename NodeType>
NodeType* Add(UEdGraph* const Graph, NodeType* const Node, const int32 X, const int32 Y)
{
    Graph->Modify();
    Graph->AddNode(Node, true, false);
    Node->CreateNewGuid();
    Node->AllocateDefaultPins();
    Node->NodePosX = X;
    Node->NodePosY = Y;
    return Node;
}

bool Link(UEdGraph* const Graph, UEdGraphPin* const From, UEdGraphPin* const To)
{
    if (From == nullptr || To == nullptr)
    {
        return false;
    }

    const UEdGraphSchema_K2* const Schema = Cast<UEdGraphSchema_K2>(Graph->GetSchema());
    if (Schema == nullptr)
    {
        return false;
    }

    if (To->Direction == EGPD_Input && To->LinkedTo.Num() > 0)
    {
        return false;
    }

    return Schema->TryCreateConnection(From, To);
}

bool LinkBackEdge(UEdGraphPin* const From, UEdGraphPin* const To)
{
    if (From == nullptr || To == nullptr || From->LinkedTo.Contains(To) || To->LinkedTo.Contains(From))
    {
        return false;
    }

    if (From->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec || To->PinType.PinCategory != UEdGraphSchema_K2::PC_Exec)
    {
        return false;
    }

    if (UEdGraphNode* const FromNode = From->GetOwningNode())
    {
        FromNode->Modify();
    }
    if (UEdGraphNode* const ToNode = To->GetOwningNode())
    {
        ToNode->Modify();
    }
    From->Modify();
    To->Modify();
    From->MakeLinkTo(To);
    return true;
}

bool Chain(UEdGraph* const Graph, UEdGraphNode* const From, UEdGraphNode* const To)
{
    return Link(Graph, Pin(From, TEXT("then"), EGPD_Output), Pin(To, TEXT("execute"), EGPD_Input));
}

UK2Node_FunctionEntry* EntryOf(UEdGraph* const Graph)
{
    for (UEdGraphNode* const Node : Graph->Nodes)
    {
        if (UK2Node_FunctionEntry* const Entry = Cast<UK2Node_FunctionEntry>(Node))
        {
            return Entry;
        }
    }
    return nullptr;
}

UK2Node_VariableGet* AddGet(UEdGraph* const Graph, const TCHAR* const Variable, const int32 X, const int32 Y)
{
    UK2Node_VariableGet* const Node = NewObject<UK2Node_VariableGet>(Graph);
    Node->VariableReference.SetSelfMember(Variable);
    return Add(Graph, Node, X, Y);
}

UK2Node_VariableSet* AddSet(UEdGraph* const Graph, const TCHAR* const Variable, const TCHAR* const Value, const int32 X, const int32 Y)
{
    UK2Node_VariableSet* const Node = NewObject<UK2Node_VariableSet>(Graph);
    Node->VariableReference.SetSelfMember(Variable);
    Add(Graph, Node, X, Y);
    if (UEdGraphPin* const ValuePin = Pin(Node, Variable, EGPD_Input))
    {
        ValuePin->DefaultValue = Value;
    }
    else
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not resolve the value pin for %s."), Variable);
        Graph->RemoveNode(Node, true);
        return nullptr;
    }
    return Node;
}

bool SetPinDefault(UEdGraphNode* const Node, const TCHAR* const Name, const FString& Value)
{
    if (UEdGraphPin* const Target = Pin(Node, Name, EGPD_Input))
    {
        Target->DefaultValue = Value;
        return true;
    }
    return false;
}

UK2Node_CallFunction* AddMath(UEdGraph* const Graph, const TCHAR* const FunctionName, const int32 X, const int32 Y)
{
    UFunction* const Function = UKismetMathLibrary::StaticClass()->FindFunctionByName(FunctionName);
    if (Function == nullptr)
    {
        // SetFromFunction(nullptr) succeeds with no pins, and the callers below then dereference the
        // pins they expect. Fail here so the authoring function reports a layout error instead.
        return nullptr;
    }

    UK2Node_CallFunction* const Node = NewObject<UK2Node_CallFunction>(Graph);
    Node->SetFromFunction(Function);
    return Add(Graph, Node, X, Y);
}

UK2Node_IfThenElse* AddBranch(UEdGraph* const Graph, const int32 X, const int32 Y)
{
    return Add(Graph, NewObject<UK2Node_IfThenElse>(Graph), X, Y);
}

// A loop header whose condition is a bool variable. The compiler inlines the read into the branch, so
// the latch targets the branch and the structurer sees a while/do-while. A computed condition cannot be
// used here: the compiler evaluates it into a temp before the branch, the latch targets that statement,
// and the structurer has no header for it yet (see the open item on computed loop conditions).
UK2Node_IfThenElse* AddBoolHeader(UEdGraph* const Graph, const TCHAR* const Variable, const int32 X, const int32 Y)
{
    UK2Node_VariableGet* const Condition = AddGet(Graph, Variable, X - 220, Y + 160);
    UK2Node_IfThenElse* const Branch = AddBranch(Graph, X, Y);
    if (Condition == nullptr || Branch == nullptr || !Link(Graph, Pin(Condition, Variable, EGPD_Output), Pin(Branch, TEXT("Condition"), EGPD_Input)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not wire the loop condition %s."), Variable);
        return nullptr;
    }
    return Branch;
}

// `Variable = IntVar < Limit`, chained after From. This is the loop's next-condition update.
UK2Node_VariableSet* AddConditionUpdate(UEdGraph* const Graph, UK2Node_VariableGet* const Counter, UEdGraphNode* const From, const TCHAR* const Variable, const int32 Limit, const int32 X, const int32 Y)
{
    UK2Node_CallFunction* const Compare = AddMath(Graph, TEXT("Less_IntInt"), X, Y + 160);
    if (Compare == nullptr)
    {
        return nullptr;
    }
    if (!SetPinDefault(Compare, TEXT("B"), FString::FromInt(Limit)) || !Link(Graph, Pin(Counter, TEXT("IntVar"), EGPD_Output), Pin(Compare, TEXT("A"), EGPD_Input)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not wire the condition compare."));
        return nullptr;
    }

    UK2Node_VariableSet* const Set = AddSet(Graph, Variable, TEXT(""), X + 260, Y);
    if (Set == nullptr || !Link(Graph, Pin(Compare, TEXT("ReturnValue"), EGPD_Output), Pin(Set, Variable, EGPD_Input)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not wire the condition update for %s."), Variable);
        return nullptr;
    }
    if (From != nullptr && !Chain(Graph, From, Set))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not chain %s to the condition update."), Variable);
        return nullptr;
    }
    return Set;
}

UK2Node_VariableSet* AddIncrement(UEdGraph* const Graph, UK2Node_VariableGet* const Counter, UEdGraphNode* const From, const int32 X, const int32 Y)
{
    const FName VariableName = Counter != nullptr ? Counter->VariableReference.GetMemberName() : NAME_None;
    if (VariableName.IsNone())
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: the increment needs a variable getter."));
        return nullptr;
    }
    const FString Variable = VariableName.ToString();
    UK2Node_CallFunction* const Add = AddMath(Graph, TEXT("Add_IntInt"), X, Y + 160);
    if (Add == nullptr)
    {
        return nullptr;
    }
    if (!SetPinDefault(Add, TEXT("B"), TEXT("1")) || !Link(Graph, Pin(Counter, *Variable, EGPD_Output), Pin(Add, TEXT("A"), EGPD_Input)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not wire the increment."));
        return nullptr;
    }

    UK2Node_VariableSet* const Set = AddSet(Graph, *Variable, TEXT(""), X + 260, Y);
    if (Set == nullptr || !Link(Graph, Pin(Add, TEXT("ReturnValue"), EGPD_Output), Pin(Set, *Variable, EGPD_Input)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not wire the increment to %s."), *Variable);
        return nullptr;
    }
    if (From != nullptr && !Chain(Graph, From, Set))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not chain the increment to IntVar."));
        return nullptr;
    }
    return Set;
}

// `if (IntVar == Value)`: the true arm leaves the body, the false arm falls through.
UK2Node_IfThenElse* AddCounterTest(UEdGraph* const Graph, UK2Node_VariableGet* const Counter, const int32 Value, const int32 X, const int32 Y)
{
    UK2Node_CallFunction* const Compare = AddMath(Graph, TEXT("EqualEqual_IntInt"), X, Y + 160);
    if (Compare == nullptr)
    {
        return nullptr;
    }
    if (!SetPinDefault(Compare, TEXT("B"), FString::FromInt(Value)) || !Link(Graph, Pin(Counter, TEXT("IntVar"), EGPD_Output), Pin(Compare, TEXT("A"), EGPD_Input)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not wire the counter test compare."));
        return nullptr;
    }

    UK2Node_IfThenElse* const Test = AddBranch(Graph, X + 260, Y);
    if (Test == nullptr || !Link(Graph, Pin(Compare, TEXT("ReturnValue"), EGPD_Output), Pin(Test, TEXT("Condition"), EGPD_Input)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: could not wire the counter test branch."));
        return nullptr;
    }
    return Test;
}

void ClearBody(UEdGraph* const Graph)
{
    TArray<UEdGraphNode*> Nodes = Graph->Nodes;
    for (UEdGraphNode* const Node : Nodes)
    {
        if (Node != nullptr && !Node->IsA<UK2Node_FunctionTerminator>())
        {
            Graph->RemoveNode(Node, true);
        }
    }
}
} // namespace

FBlueprintOpcodeFixtureBuilder::FBlueprintOpcodeFixtureBuilder(UBlueprint& InBlueprint) : Blueprint(InBlueprint)
{
}

UEdGraph* FBlueprintOpcodeFixtureBuilder::FindOrCreateGraph(const TCHAR* const GraphName)
{
    for (UEdGraph* Graph : Blueprint.FunctionGraphs)
    {
        if (Graph && Graph->GetFName() == GraphName)
        {
            return Graph;
        }
    }

    UEdGraph* const Graph = FBlueprintEditorUtils::CreateNewGraph(&Blueprint, FName(GraphName), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (Graph != nullptr)
    {
        FBlueprintEditorUtils::AddFunctionGraph<UClass>(&Blueprint, Graph, true, nullptr);
    }
    return Graph;
}

void FBlueprintOpcodeFixtureBuilder::CompactLayout(UEdGraph& Graph)
{
    TArray<UEdGraphNode*> Body;
    UK2Node_FunctionEntry* Entry = nullptr;
    UK2Node_FunctionResult* Result = nullptr;
    for (UEdGraphNode* const Node : Graph.Nodes)
    {
        if (Node == nullptr)
        {
            continue;
        }
        if (UK2Node_FunctionEntry* const AsEntry = Cast<UK2Node_FunctionEntry>(Node))
        {
            Entry = AsEntry;
        }
        else if (UK2Node_FunctionResult* const AsResult = Cast<UK2Node_FunctionResult>(Node))
        {
            Result = AsResult;
        }
        else
        {
            Body.Add(Node);
        }
    }

    if (Body.Num() == 0)
    {
        if (Entry != nullptr)
        {
            Entry->NodePosX = 0;
            Entry->NodePosY = 0;
        }
        if (Result != nullptr)
        {
            Result->NodePosX = 260;
            Result->NodePosY = 0;
        }
        return;
    }

    TArray<int32> Xs;
    TArray<int32> Ys;
    for (const UEdGraphNode* const Node : Body)
    {
        Xs.Add(Node->NodePosX);
        Ys.Add(Node->NodePosY);
    }
    Xs.Sort();
    Ys.Sort();

    const auto CompactAxis = [](const TArray<int32>& Values, const int32 MaxGap) {
        TMap<int32, int32> Positions;
        if (Values.Num() == 0)
        {
            return Positions;
        }
        int32 Cursor = 0;
        int32 Previous = Values[0];
        Positions.Add(Previous, Cursor);
        for (int32 Index = 1; Index < Values.Num(); ++Index)
        {
            if (Values[Index] == Previous)
            {
                continue;
            }
            Cursor += FMath::Min(Values[Index] - Previous, MaxGap);
            Previous = Values[Index];
            Positions.Add(Previous, Cursor);
        }
        return Positions;
    };

    const TMap<int32, int32> CompactX = CompactAxis(Xs, 260);
    const TMap<int32, int32> CompactY = CompactAxis(Ys, 180);
    const int32 BodyOffset = Entry != nullptr ? 260 : 0;
    int32 MaxX = 0;
    for (UEdGraphNode* const Node : Body)
    {
        Node->NodePosX = CompactX.FindRef(Node->NodePosX) + BodyOffset;
        Node->NodePosY = CompactY.FindRef(Node->NodePosY);
        MaxX = FMath::Max(MaxX, Node->NodePosX);
    }

    if (Entry != nullptr)
    {
        Entry->NodePosX = 0;
        Entry->NodePosY = 0;
    }
    if (Result != nullptr)
    {
        Result->NodePosX = MaxX + 260;
        Result->NodePosY = 0;
    }
}

bool FBlueprintOpcodeFixtureBuilder::EnsureLoopFlag()
{
    if (FBlueprintEditorUtils::FindMemberVariableGuidByName(&Blueprint, TEXT("LoopFlag")).IsValid())
    {
        return true;
    }

    FEdGraphPinType Type;
    Type.PinCategory = UEdGraphSchema_K2::PC_Boolean;
    return FBlueprintEditorUtils::AddMemberVariable(&Blueprint, TEXT("LoopFlag"), Type);
}

bool FBlueprintOpcodeFixtureBuilder::EnsureDelegateVariable()
{
    const FDelegateProperty* const SourceProperty = FindFProperty<FDelegateProperty>(UOpcodeProbeHost::StaticClass(), TEXT("SingleDelegateVar"));
    if (SourceProperty == nullptr || SourceProperty->SignatureFunction == nullptr)
    {
        return false;
    }

    const UEdGraphSchema_K2* const Schema = GetDefault<UEdGraphSchema_K2>();
    FEdGraphPinType Type;
    if (Schema == nullptr || !Schema->ConvertPropertyToPinType(SourceProperty, Type))
    {
        return false;
    }

    // Any existing Blueprint-authored delegate variable is the one that disappears on save.
    const int32 ExistingIndex = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, TEXT("SingleDelegateVar"));
    if (ExistingIndex == INDEX_NONE)
    {
        return FBlueprintEditorUtils::AddMemberVariable(&Blueprint, TEXT("SingleDelegateVar"), Type);
    }

    FBPVariableDescription& Existing = Blueprint.NewVariables[ExistingIndex];
    if (Existing.VarType != Type)
    {
        Blueprint.Modify();
        Existing.VarType = Type;
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
    }
    return true;
}

bool FBlueprintOpcodeFixtureBuilder::EnsureEnumVariable()
{
    UEnum* const Enum = LoadObject<UEnum>(nullptr, TEXT("/BlueprintInspector/Tests/OpcodeShowcase/E_OpcodeDemoEnum.E_OpcodeDemoEnum"));
    if (Enum == nullptr)
    {
        return false;
    }

    if (FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, TEXT("EnumVar")) != INDEX_NONE)
    {
        return true;
    }

    // The schema's UI category is "enum", but the stored pin type is PC_Byte plus the UEnum object.
    FEdGraphPinType Type;
    Type.PinCategory = UEdGraphSchema_K2::PC_Byte;
    Type.PinSubCategoryObject = Enum;
    return FBlueprintEditorUtils::AddMemberVariable(&Blueprint, TEXT("EnumVar"), Type);
}

bool FBlueprintOpcodeFixtureBuilder::EnsureWeakVariable()
{
    const int32 VariableIndex = FBlueprintEditorUtils::FindNewVariableIndex(&Blueprint, TEXT("WeakObjVar"));
    if (VariableIndex == INDEX_NONE)
    {
        // This restores a flag on a variable the bootstrap authors; it cannot create the variable, so a
        // miss is a real failure rather than a reason to skip the row silently.
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: WeakObjVar does not exist; author it before restoring its weak flag."));
        return false;
    }

    // AddMemberVariable clears bIsWeakPointer for user-created variables, so the flag has to be restored
    // after the variable exists. The compiler reads it from the variable description when it creates the
    // FWeakObjectProperty, and from the destination pin when it chooses the Let token.
    const bool bNeedsWeakFlagRestore = !Blueprint.NewVariables[VariableIndex].VarType.bIsWeakPointer;
    if (bNeedsWeakFlagRestore)
    {
        Blueprint.Modify();
        Blueprint.NewVariables[VariableIndex].VarType.bIsWeakPointer = true;
    }

    bool bFoundGraph = false;
    bool bTouchedPins = false;
    for (UEdGraph* const Graph : Blueprint.FunctionGraphs)
    {
        if (Graph == nullptr || Graph->GetFName() != FName(TEXT("let_weakobj")))
        {
            continue;
        }

        bFoundGraph = true;
        for (UEdGraphNode* const Node : Graph->Nodes)
        {
            for (UEdGraphPin* const GraphPin : Node->Pins)
            {
                if (GraphPin != nullptr && GraphPin->PinName == FName(TEXT("WeakObjVar")) && (GraphPin->PinType.PinCategory == UEdGraphSchema_K2::PC_Object || GraphPin->PinType.PinCategory == UEdGraphSchema_K2::PC_SoftObject) && !GraphPin->PinType.bIsWeakPointer)
                {
                    Node->Modify();
                    GraphPin->PinType.bIsWeakPointer = true;
                    bTouchedPins = true;
                }
            }
        }
    }

    if (!bFoundGraph)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: the let_weakobj graph is missing; the weak-reference row cannot be authored."));
        return false;
    }
    if (bTouchedPins || bNeedsWeakFlagRestore)
    {
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(&Blueprint);
    }

    return true;
}

bool FBlueprintOpcodeFixtureBuilder::EnsureEnumAsset()
{
    UUserDefinedEnum* const Enum = LoadObject<UUserDefinedEnum>(nullptr, TEXT("/BlueprintInspector/Tests/OpcodeShowcase/E_OpcodeDemoEnum.E_OpcodeDemoEnum"));
    if (Enum == nullptr)
    {
        return false;
    }

    // The guide specifies One/Two/Three. NumEnums() includes the hidden _MAX entry, so four entries
    // are the three values plus _MAX. Bound the add attempts and verify the count: a failed add would
    // otherwise spin this loop forever.
    int32 AddAttempts = 0;
    while (Enum->NumEnums() < 4 && AddAttempts < 4)
    {
        FEnumEditorUtils::AddNewEnumeratorForUserDefinedEnum(Enum);
        ++AddAttempts;
    }
    if (Enum->NumEnums() < 4)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: E_OpcodeDemoEnum could not be extended to 4 entries."));
        return false;
    }

    UPackage* const Package = Enum->GetOutermost();
    if (Package == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: E_OpcodeDemoEnum has no package."));
        return false;
    }
    const bool bWasDirty = Package->IsDirty();

    bool bChanged = false;
    TArray<int32> ChangedIndices;
    TArray<FString> PreviousNames;
    const TCHAR* const DisplayNames[] = {TEXT("One"), TEXT("Two"), TEXT("Three")};
    for (int32 Index = 0; Index < UE_ARRAY_COUNT(DisplayNames); ++Index)
    {
        const FString CurrentName = Enum->GetDisplayNameTextByIndex(Index).ToString();
        if (CurrentName == DisplayNames[Index])
        {
            continue;
        }
        if (!CurrentName.StartsWith(TEXT("NewEnumerator")))
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: E_OpcodeDemoEnum entry %d is already named %s; refusing to overwrite it."), Index, *CurrentName);
            return false;
        }
        ChangedIndices.Add(Index);
        PreviousNames.Add(CurrentName);
        FEnumEditorUtils::SetEnumeratorDisplayName(Enum, Index, FText::FromString(DisplayNames[Index]));
        bChanged = true;
    }
    if (!bChanged)
    {
        return true;
    }

    Package->MarkPackageDirty();
    const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());

#if BI_ENGINE_AT_LEAST_5
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    const bool bSaved = UPackage::SavePackage(Package, Enum, *FileName, SaveArgs);
#else
    const bool bSaved = UPackage::SavePackage(Package, Enum, RF_Public | RF_Standalone, *FileName, GError, nullptr, false, true, SAVE_NoError);
#endif

    if (!bSaved)
    {
        for (int32 Index = 0; Index < ChangedIndices.Num(); ++Index)
        {
            FEnumEditorUtils::SetEnumeratorDisplayName(Enum, ChangedIndices[Index], FText::FromString(PreviousNames[Index]));
        }
        Package->SetDirtyFlag(bWasDirty);
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: E_OpcodeDemoEnum could not be saved; the display names were rolled back."));
    }
    return bSaved;
}

bool FBlueprintOpcodeFixtureBuilder::AuthorSwitch(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UK2Node_SwitchInteger* const Switch = NewObject<UK2Node_SwitchInteger>(&Graph);
    Add(&Graph, Switch, 550, 0);
    Switch->AddPinToSwitchNode();
    Switch->AddPinToSwitchNode();

    UK2Node_VariableGet* const Selection = AddGet(&Graph, TEXT("IntVar"), 250, 200);
    const bool bSelectionLinked = Link(&Graph, Pin(Selection, TEXT("IntVar"), EGPD_Output), Pin(Switch, TEXT("Selection"), EGPD_Input));

    UK2Node_VariableSet* const CaseZero = AddSet(&Graph, TEXT("RealVar"), TEXT("0.0"), 950, -150);
    UK2Node_VariableSet* const CaseOne = AddSet(&Graph, TEXT("RealVar"), TEXT("1.0"), 950, 0);
    UK2Node_VariableSet* const DefaultCase = AddSet(&Graph, TEXT("RealVar"), TEXT("2.0"), 950, 150);

    return bSelectionLinked && Chain(&Graph, Entry, Switch) && Link(&Graph, Pin(Switch, TEXT("0"), EGPD_Output), Pin(CaseZero, TEXT("execute"), EGPD_Input)) && Link(&Graph, Pin(Switch, TEXT("1"), EGPD_Output), Pin(CaseOne, TEXT("execute"), EGPD_Input)) && Link(&Graph, Pin(Switch, TEXT("Default"), EGPD_Output), Pin(DefaultCase, TEXT("execute"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorCallerArgument(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UFunction* const Function = UOpcodeProbeHost::StaticClass()->FindFunctionByName(TEXT("TakeCaller"));
    if (Function == nullptr)
    {
        return false;
    }

    UK2Node_CallFunction* const Call = NewObject<UK2Node_CallFunction>(&Graph);
    Call->SetFromFunction(Function);
    Add(&Graph, Call, 550, 0);

    UK2Node_Self* const Self = Add(&Graph, NewObject<UK2Node_Self>(&Graph), 250, 160);

    return Chain(&Graph, Entry, Call) && SetPinDefault(Call, TEXT("Value"), TEXT("7")) && Link(&Graph, Pin(Self, TEXT("self"), EGPD_Output), Pin(Call, TEXT("Caller"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorBranch(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UK2Node_IfThenElse* const Branch = AddBranch(&Graph, 500, 0);
    UK2Node_VariableGet* const Condition = AddGet(&Graph, TEXT("BoolVar"), 200, 160);
    UK2Node_VariableSet* const ThenSet = AddSet(&Graph, TEXT("IntVar"), TEXT("0"), 800, -120);
    UK2Node_VariableSet* const ElseSet = AddSet(&Graph, TEXT("IntVar"), TEXT("0"), 800, 120);

    return Chain(&Graph, Entry, Branch) && Link(&Graph, Pin(Condition, TEXT("BoolVar"), EGPD_Output), Pin(Branch, TEXT("Condition"), EGPD_Input)) && Chain(&Graph, Branch, ThenSet) && Link(&Graph, Pin(Branch, TEXT("else"), EGPD_Output), Pin(ElseSet, TEXT("execute"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorWhile(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    // Unlike the other loop fixtures this one keeps a computed condition: the compiler evaluates
    // Less_IntInt into a temp before the branch, so the latch (`then`) targets the temp assignment and
    // the structurer has no header for it. That is deliberate -- this row is the computed-condition
    // while case, and its opcode stream differs from AuthorDoWhile's bool-variable header.
    UK2Node_VariableGet* const Counter = AddGet(&Graph, TEXT("IntVar"), 500, 300);
    UK2Node_VariableSet* const Init = AddSet(&Graph, TEXT("IntVar"), TEXT("0"), 150, 0);

    UK2Node_CallFunction* const Compare = AddMath(&Graph, TEXT("Less_IntInt"), 550, 160);
    if (Compare == nullptr)
    {
        return false;
    }
    const bool bCompareDefault = SetPinDefault(Compare, TEXT("B"), TEXT("3"));
    const bool bCompareLinked = Link(&Graph, Pin(Counter, TEXT("IntVar"), EGPD_Output), Pin(Compare, TEXT("A"), EGPD_Input));

    UK2Node_IfThenElse* const Header = AddBranch(&Graph, 850, 0);
    const bool bConditionLinked = Link(&Graph, Pin(Compare, TEXT("ReturnValue"), EGPD_Output), Pin(Header, TEXT("Condition"), EGPD_Input));

    UK2Node_VariableSet* const Body = AddIncrement(&Graph, Counter, Header, 1150, 100);

    return bCompareDefault && bCompareLinked && bConditionLinked && Chain(&Graph, Entry, Init) && Chain(&Graph, Init, Header) && LinkBackEdge(Pin(Body, TEXT("then"), EGPD_Output), Pin(Header, TEXT("execute"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorDoWhile(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UK2Node_VariableGet* const Counter = AddGet(&Graph, TEXT("IntVar"), 500, 300);
    UK2Node_VariableSet* const Init = AddSet(&Graph, TEXT("IntVar"), TEXT("0"), 150, 0);
    UK2Node_VariableSet* const Body = AddIncrement(&Graph, Counter, Init, 400, 100);
    UK2Node_VariableSet* const Next = AddConditionUpdate(&Graph, Counter, Body, TEXT("BoolVar"), 3, 750, 100);
    UK2Node_IfThenElse* const Tail = AddBoolHeader(&Graph, TEXT("BoolVar"), 1200, 0);

    return Chain(&Graph, Entry, Init) && Chain(&Graph, Next, Tail) && LinkBackEdge(Pin(Tail, TEXT("then"), EGPD_Output), Pin(Body, TEXT("execute"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorContinue(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UK2Node_VariableGet* const Counter = AddGet(&Graph, TEXT("IntVar"), 500, 300);
    UK2Node_VariableSet* const Init = AddSet(&Graph, TEXT("IntVar"), TEXT("0"), 150, 0);
    UK2Node_VariableSet* const Start = AddSet(&Graph, TEXT("BoolVar"), TEXT("true"), 350, 0);
    UK2Node_IfThenElse* const Header = AddBoolHeader(&Graph, TEXT("BoolVar"), 750, 0);
    UK2Node_VariableSet* const Body = AddIncrement(&Graph, Counter, Header, 1050, 100);
    UK2Node_IfThenElse* const ContinueTest = AddCounterTest(&Graph, Counter, 1, 1400, 100);
    UK2Node_VariableSet* const Next = AddConditionUpdate(&Graph, Counter, nullptr, TEXT("BoolVar"), 3, 1800, 200);
    const bool bBodyChained = Chain(&Graph, Body, ContinueTest);
    const bool bNextLinked = Link(&Graph, Pin(ContinueTest, TEXT("else"), EGPD_Output), Pin(Next, TEXT("execute"), EGPD_Input));

    return bBodyChained && bNextLinked && Chain(&Graph, Entry, Init) && Chain(&Graph, Init, Start) && Chain(&Graph, Start, Header) && LinkBackEdge(Pin(ContinueTest, TEXT("then"), EGPD_Output), Pin(Header, TEXT("execute"), EGPD_Input)) && LinkBackEdge(Pin(Next, TEXT("then"), EGPD_Output), Pin(Header, TEXT("execute"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorBreak(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UK2Node_VariableGet* const Counter = AddGet(&Graph, TEXT("IntVar"), 500, 300);
    UK2Node_VariableSet* const Init = AddSet(&Graph, TEXT("IntVar"), TEXT("0"), 150, 0);
    UK2Node_VariableSet* const Start = AddSet(&Graph, TEXT("BoolVar"), TEXT("true"), 350, 0);
    UK2Node_IfThenElse* const Header = AddBoolHeader(&Graph, TEXT("BoolVar"), 750, 0);
    UK2Node_VariableSet* const Body = AddIncrement(&Graph, Counter, Header, 1050, 100);
    UK2Node_IfThenElse* const BreakTest = AddCounterTest(&Graph, Counter, 2, 1400, 100);
    UK2Node_VariableSet* const Next = AddConditionUpdate(&Graph, Counter, nullptr, TEXT("BoolVar"), 3, 1800, 200);
    const bool bBodyChained = Chain(&Graph, Body, BreakTest);
    const bool bNextLinked = Link(&Graph, Pin(BreakTest, TEXT("else"), EGPD_Output), Pin(Next, TEXT("execute"), EGPD_Input));

    return bBodyChained && bNextLinked && Chain(&Graph, Entry, Init) && Chain(&Graph, Init, Start) && Chain(&Graph, Start, Header) && LinkBackEdge(Pin(Next, TEXT("then"), EGPD_Output), Pin(Header, TEXT("execute"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorNestedLoops(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UK2Node_VariableGet* const Counter = AddGet(&Graph, TEXT("IntVar"), 500, 300);
    UK2Node_VariableSet* const Init = AddSet(&Graph, TEXT("IntVar"), TEXT("0"), 100, 0);
    UK2Node_VariableSet* const StartOuter = AddSet(&Graph, TEXT("LoopFlag"), TEXT("true"), 300, 0);
    UK2Node_IfThenElse* const Outer = AddBoolHeader(&Graph, TEXT("LoopFlag"), 650, 0);
    UK2Node_VariableSet* const StartInner = AddSet(&Graph, TEXT("BoolVar"), TEXT("true"), 950, 0);
    UK2Node_IfThenElse* const Inner = AddBoolHeader(&Graph, TEXT("BoolVar"), 1250, 0);
    UK2Node_VariableSet* const Body = AddIncrement(&Graph, Counter, Inner, 1550, 100);
    UK2Node_IfThenElse* const ContinueTest = AddCounterTest(&Graph, Counter, 1, 1900, 100);
    UK2Node_IfThenElse* const BreakTest = AddCounterTest(&Graph, Counter, 2, 2300, 100);
    UK2Node_VariableSet* const AfterInner = AddSet(&Graph, TEXT("LoopFlag"), TEXT("false"), 2700, 100);
    const bool bBodyChained = Chain(&Graph, Body, ContinueTest);
    const bool bElseLinked = Link(&Graph, Pin(ContinueTest, TEXT("else"), EGPD_Output), Pin(BreakTest, TEXT("execute"), EGPD_Input));

    return bBodyChained && bElseLinked && Chain(&Graph, Entry, Init) && Chain(&Graph, Init, StartOuter) && Chain(&Graph, StartOuter, Outer) && Chain(&Graph, Outer, StartInner) && Chain(&Graph, StartInner, Inner) && LinkBackEdge(Pin(ContinueTest, TEXT("then"), EGPD_Output), Pin(Inner, TEXT("execute"), EGPD_Input)) && LinkBackEdge(Pin(BreakTest, TEXT("else"), EGPD_Output), Pin(Inner, TEXT("execute"), EGPD_Input)) && LinkBackEdge(Pin(BreakTest, TEXT("then"), EGPD_Output), Pin(AfterInner, TEXT("execute"), EGPD_Input)) && LinkBackEdge(Pin(Inner, TEXT("else"), EGPD_Output), Pin(AfterInner, TEXT("execute"), EGPD_Input)) && LinkBackEdge(Pin(AfterInner, TEXT("then"), EGPD_Output), Pin(Outer, TEXT("execute"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorContextFailSilent(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    // A native, non-virtual function with no return value and no non-const out parameter keeps
    // bUnsafeToSkip false, which is what lets the context emitter choose EX_Context_FailSilent when
    // Kismet.bCanSuppressAccessViolation is true.
    UK2Node_CallFunction* const Call = NewObject<UK2Node_CallFunction>(&Graph);
    Call->FunctionReference.SetExternalMember(TEXT("SetActorScale3D"), AActor::StaticClass());
    Add(&Graph, Call, 700, 0);

    UK2Node_VariableGet* const Actor = AddGet(&Graph, TEXT("ActorVar"), 250, 160);
    if (UEdGraphPin* const ScalePin = Pin(Call, TEXT("NewScale3D"), EGPD_Input))
    {
        ScalePin->DefaultValue = TEXT("1, 1, 1");
    }

    return Chain(&Graph, Entry, Call) && Link(&Graph, Pin(Actor, TEXT("ActorVar"), EGPD_Output), Pin(Call, TEXT("self"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorLetDelegate(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    // UK2Node_DelegateSet is an event entry point and cannot live in a function body; a delegate-typed
    // VariableSet is the function form, and the compiler still lowers it to EX_LetDelegate.
    UK2Node_VariableSet* const Set = AddSet(&Graph, TEXT("SingleDelegateVar"), TEXT(""), 900, 0);
    UK2Node_CreateDelegate* const Create = NewObject<UK2Node_CreateDelegate>(&Graph);

    // SetFunction still allocates the delegate pin when the name does not resolve, and the compiler
    // only reports the miss later, so validate against the class the node will bind to first.
    UClass* const SearchClass = Blueprint.SkeletonGeneratedClass != nullptr ? Blueprint.SkeletonGeneratedClass : Blueprint.GeneratedClass;
    if (SearchClass == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: %s has neither a skeleton nor a generated class; the delegate row cannot bind."), *Blueprint.GetName());
        return false;
    }
    if (SearchClass->FindFunctionByName(TEXT("F_ProbeDelegateHandler")) == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: F_ProbeDelegateHandler does not exist on %s; the delegate row cannot bind."), *SearchClass->GetName());
        return false;
    }
    Create->SetFunction(TEXT("F_ProbeDelegateHandler"));
    Add(&Graph, Create, 450, 160);

    return Chain(&Graph, Entry, Set) && Link(&Graph, Pin(Create, TEXT("OutputDelegate"), EGPD_Output), Pin(Set, TEXT("SingleDelegateVar"), EGPD_Input));
}

bool FBlueprintOpcodeFixtureBuilder::AuthorDelegateCreateDelegate(UEdGraph& Graph)
{
    // The subject of this fixture is the Create Delegate node feeding the set.
    return AuthorLetDelegate(Graph);
}

bool FBlueprintOpcodeFixtureBuilder::EnsureDelegateLibraryFunction()
{
    UBlueprint* const Library = LoadObject<UBlueprint>(nullptr, TEXT("/BlueprintInspector/Tests/OpcodeShowcase/BFL_ProbeLibrary.BFL_ProbeLibrary"));
    if (Library == nullptr || Library->GeneratedClass == nullptr)
    {
        return false;
    }

    if (Library->GeneratedClass->FindFunctionByName(TEXT("SingleDelegate")) != nullptr)
    {
        return true;
    }

    const FDelegateProperty* const SourceProperty = FindFProperty<FDelegateProperty>(UOpcodeProbeHost::StaticClass(), TEXT("SingleDelegateVar"));
    if (SourceProperty == nullptr || SourceProperty->SignatureFunction == nullptr)
    {
        return false;
    }

    const UEdGraphSchema_K2* const Schema = GetDefault<UEdGraphSchema_K2>();
    FEdGraphPinType Type;
    if (Schema == nullptr || !Schema->ConvertPropertyToPinType(SourceProperty, Type))
    {
        return false;
    }

    for (UEdGraph* const Existing : Library->FunctionGraphs)
    {
        if (Existing != nullptr && Existing->GetFName() == FName(TEXT("SingleDelegate")))
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: the SingleDelegate graph already exists on %s but is not compiled into the generated class."), *Library->GetName());
            return false;
        }
    }

    UEdGraph* const Graph = FBlueprintEditorUtils::CreateNewGraph(Library, FName(TEXT("SingleDelegate")), UEdGraph::StaticClass(), UEdGraphSchema_K2::StaticClass());
    if (Graph == nullptr)
    {
        return false;
    }
    FBlueprintEditorUtils::AddFunctionGraph<UClass>(Library, Graph, true, nullptr);

    UK2Node_FunctionEntry* const Entry = EntryOf(Graph);
    if (Entry == nullptr || Entry->CreateUserDefinedPin(TEXT("Del"), Type, EGPD_Output) == nullptr)
    {
        FBlueprintEditorUtils::RemoveGraph(Library, Graph);
        return false;
    }

    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Library);
    FKismetEditorUtilities::CompileBlueprint(Library);
    if (Library->Status == BS_Error)
    {
        FBlueprintEditorUtils::RemoveGraph(Library, Graph);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Library);
        return false;
    }

    UPackage* const Package = Library->GetOutermost();
    if (Package == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Fixture builder: %s has no package to save."), *Library->GetName());
        return false;
    }
    Package->MarkPackageDirty();
    const FString FileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
#if BI_ENGINE_AT_LEAST_5
    FSavePackageArgs SaveArgs;
    SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
    SaveArgs.SaveFlags = SAVE_NoError;
    const bool bSaved = UPackage::SavePackage(Package, Library, *FileName, SaveArgs);
#else
    const bool bSaved = UPackage::SavePackage(Package, Library, RF_Public | RF_Standalone, *FileName, GError, nullptr, false, true, SAVE_NoError);
#endif
    if (!bSaved)
    {
        FBlueprintEditorUtils::RemoveGraph(Library, Graph);
        FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Library);
    }
    return bSaved;
}

bool FBlueprintOpcodeFixtureBuilder::AuthorDelegateInstance(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UClass* const LibraryClass = LoadObject<UClass>(nullptr, TEXT("/BlueprintInspector/Tests/OpcodeShowcase/BFL_ProbeLibrary.BFL_ProbeLibrary_C"));
    UFunction* const Callee = LibraryClass != nullptr ? LibraryClass->FindFunctionByName(TEXT("SingleDelegate")) : nullptr;
    if (Callee == nullptr)
    {
        return false;
    }

    UK2Node_CallFunction* const Call = NewObject<UK2Node_CallFunction>(&Graph);
    Call->SetFromFunction(Callee);
    Add(&Graph, Call, 700, 0);

    return Chain(&Graph, Entry, Call);
}

bool FBlueprintOpcodeFixtureBuilder::AuthorEventGraphFixtures()
{
    if (Blueprint.UbergraphPages.Num() == 0 || Blueprint.UbergraphPages[0] == nullptr)
    {
        return false;
    }

    UEdGraph* const Graph = Blueprint.UbergraphPages[0];

    TArray<UEdGraphNode*> ExistingNodes = Graph->Nodes;
    for (UEdGraphNode* const Node : ExistingNodes)
    {
        if (Node != nullptr)
        {
            Graph->RemoveNode(Node, true);
        }
    }

    // The output pin is the only producer of EX_LetValueOnPersistentFrame: the compiler stores the
    // event's out value in the persistent ubergraph frame before the body reads it.
    UK2Node_CustomEvent* const WithData = NewObject<UK2Node_CustomEvent>(Graph);
    WithData->CustomFunctionName = TEXT("E_WithDataOut");
    Add(Graph, WithData, 300, 0);

    FEdGraphPinType IntType;
    IntType.PinCategory = UEdGraphSchema_K2::PC_Int;
    UEdGraphPin* const OutPin = WithData->CreateUserDefinedPin(TEXT("Out"), IntType, EGPD_Output);

    UK2Node_VariableSet* const StoreOut = AddSet(Graph, TEXT("IntVar"), TEXT("0"), 900, 0);
    const bool bOutLink = OutPin != nullptr && Link(Graph, OutPin, Pin(StoreOut, TEXT("IntVar"), EGPD_Input));
    const bool bOutChain = bOutLink && Chain(Graph, WithData, StoreOut);

    // A second custom event keeps the ubergraph entry points distinct; any event would do.
    UK2Node_CustomEvent* const AnyEvent = NewObject<UK2Node_CustomEvent>(Graph);
    AnyEvent->CustomFunctionName = TEXT("E_Any");
    Add(Graph, AnyEvent, 300, 300);

    UK2Node_VariableSet* const StoreAny = AddSet(Graph, TEXT("IntVar"), TEXT("0"), 900, 300);
    const bool bAnyLinked = Chain(Graph, AnyEvent, StoreAny);

    CompactLayout(*Graph);
    return bOutChain && bAnyLinked;
}

bool FBlueprintOpcodeFixtureBuilder::AuthorLetEnum(UEdGraph& Graph)
{
    ClearBody(&Graph);

    UK2Node_FunctionEntry* const Entry = EntryOf(&Graph);
    if (Entry == nullptr)
    {
        return false;
    }

    UEnum* const Enum = LoadObject<UEnum>(nullptr, TEXT("/BlueprintInspector/Tests/OpcodeShowcase/E_OpcodeDemoEnum.E_OpcodeDemoEnum"));
    if (Enum == nullptr || Enum->NumEnums() <= 1)
    {
        return false;
    }

    // The pin default is the enum's internal entry name (NewEnumerator1), not the display name; the
    // schema validates it with GetIndexByNameString.
    const FString EntryName = Enum->GetNameStringByIndex(1);
    UK2Node_VariableSet* const Set = AddSet(&Graph, TEXT("EnumVar"), *EntryName, 900, 0);
    return Chain(&Graph, Entry, Set);
}

#endif // WITH_EDITOR
