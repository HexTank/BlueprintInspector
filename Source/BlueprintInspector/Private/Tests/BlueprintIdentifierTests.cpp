// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"

#include "BlueprintFixtureSource.h"
#include "BlueprintScriptDisassembler.h"
#include "BlueprintScriptVisualizer_Pseudo.h"
#include "BlueprintSourceMap.h"

#include "UObject/UnrealType.h"

// Blueprint names are free-form ("Hit Actor", "Dot Product - Forward Hit") but rendered code has to be an
// identifier, and the navigation model relies on there being exactly one spelling. Pin the mapping.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintIdentifierSanitisationTest, "BlueprintInspector.Rendering.IdentifierSanitisation", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintIdentifierSanitisationTest::RunTest(const FString& Parameters)
{
    using FVisualizer = FBlueprintScriptVisualizer_Pseudo;

    TestEqual(TEXT("a space becomes an underscore"), FVisualizer::SanitiseIdentifier(TEXT("Hit Actor")), FString(TEXT("Hit_Actor")));
    TestEqual(TEXT("punctuation becomes an underscore"), FVisualizer::SanitiseIdentifier(TEXT("Dot Product - Forward Hit")), FString(TEXT("Dot_Product_Forward_Hit")));
    TestEqual(TEXT("a run of separators collapses to one underscore"), FVisualizer::SanitiseIdentifier(TEXT("A  -  B")), FString(TEXT("A_B")));
    TestEqual(TEXT("a qualified name keeps its separator"), FVisualizer::SanitiseIdentifier(TEXT("BP_Foo::Hit Actor")), FString(TEXT("BP_Foo::Hit_Actor")));
    TestEqual(TEXT("an identifier is unchanged"), FVisualizer::SanitiseIdentifier(TEXT("CallFunc_CheckAmmoState_ReturnValue")), FString(TEXT("CallFunc_CheckAmmoState_ReturnValue")));
    TestEqual(TEXT("a run of underscores in an identifier collapses too"), FVisualizer::SanitiseIdentifier(TEXT("CallFunc__CheckAmmoState")), FString(TEXT("CallFunc_CheckAmmoState")));
    TestEqual(TEXT("an empty name still yields an identifier"), FVisualizer::SanitiseIdentifier(FString()), FString(TEXT("_")));
    TestEqual(TEXT("a leading digit is prefixed"), FVisualizer::SanitiseIdentifier(TEXT("1403 Entry")), FString(TEXT("_1403_Entry")));

    return true;
}

// The statement renderer sanitises a call name; every expression branch that prints a node name must do
// the same, or one node renders as valid text as a statement and as invalid text as an operand. These
// cases pin every such branch.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintExpressionNameSanitisationTest, "BlueprintInspector.Rendering.ExpressionNamesAreIdentifiers", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintExpressionNameSanitisationTest::RunTest(const FString& Parameters)
{
    using FVisualizer = FBlueprintScriptVisualizer_Pseudo;

    const FName FreeFormName(TEXT("Dot Product - Forward Hit"));
    const FString Expected = FString::Printf(TEXT("%s()"), *FVisualizer::SanitiseIdentifier(FreeFormName.ToString()));

    // The branches that print the node's name: the call group, a SwitchValue with no input operand, and
    // the default arm every other opcode falls through to.
    const EExprToken Opcodes[] = {EX_FinalFunction, EX_CallMath, EX_VirtualFunction, EX_LocalVirtualFunction, EX_LocalFinalFunction, EX_StructConst, EX_ArrayConst, EX_SetConst, EX_MapConst, EX_SetArray, EX_SwitchValue, EX_Let};
    for (const EExprToken Opcode : Opcodes)
    {
        FBlueprintNode Node(TWeakObjectPtr<const UFunction>(), FreeFormName, Opcode);
        FVisualizer Visualizer;
        const FString Rendered{Visualizer(&Node)};

        TestEqual(FString::Printf(TEXT("opcode 0x%.2X renders the sanitised name"), Opcode), Rendered, Expected);
        TestFalse(FString::Printf(TEXT("opcode 0x%.2X emits no space"), Opcode), Rendered.Contains(TEXT(" ")));
    }

    return true;
}

// The fixture's event graph carries two custom events, and a class ubergraph renders one section per entry
// point. Pin both the fixture and the rule that keeps those sections distinguishable.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintUbergraphEntryNamingTest, "BlueprintInspector.Rendering.UbergraphEntryNaming", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintUbergraphEntryNamingTest::RunTest(const FString& Parameters)
{
    UClass* const Class = BlueprintFixtureSource::GetMainClass();
    if (Class == nullptr)
    {
        AddError(TEXT("The transient fixture has no generated class."));
        return false;
    }

    const UFunction* Ubergraph = nullptr;
    bool bHasWithData = false;
    bool bHasAny = false;
    for (TFieldIterator<UFunction> It(Class, EFieldIteratorFlags::ExcludeSuper); It; ++It)
    {
        const FString Name = It->GetName();
        if (Name.StartsWith(TEXT("ExecuteUbergraph")))
        {
            if (Ubergraph != nullptr)
            {
                AddError(TEXT("The fixture has more than one ubergraph function."));
                return false;
            }
            Ubergraph = *It;
        }
        bHasWithData = bHasWithData || Name == TEXT("E_WithDataOut");
        bHasAny = bHasAny || Name == TEXT("E_Any");
    }

    if (!TestNotNull(TEXT("the fixture has an ubergraph function"), Ubergraph))
    {
        return false;
    }
    TestTrue(TEXT("the fixture has the first custom event"), bHasWithData);
    TestTrue(TEXT("the fixture has the second custom event"), bHasAny);

    const FString Signature = FString::Printf(TEXT("%s(int32 EntryPoint)"), *Ubergraph->GetName());
    const FString FirstEntry = UbergraphEntrySignature(Signature, 1403);
    const FString SecondEntry = UbergraphEntrySignature(Signature, 15);

    TestEqual(TEXT("the offset is inserted before the parenthesis"), FirstEntry, FString::Printf(TEXT("%s_1403(int32 EntryPoint)"), *Ubergraph->GetName()));
    TestNotEqual(TEXT("two entry points do not share a name"), FirstEntry, SecondEntry);
    TestEqual(TEXT("a signature without a parenthesis still carries the entry suffix"), UbergraphEntrySignature(TEXT("Plain"), 7), FString(TEXT("Plain_7")));

    return true;
}

#endif
