// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "BlueprintExpression.h"
#include "BlueprintFixtureSource.h"
#include "OpcodeProbeHost.h"

#include "UObject/Class.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UnrealType.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintExpressionPathTest, "BlueprintInspector.Expression.Paths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintExpressionPathTest::RunTest(const FString& Parameters)
{
    UClass* const Class = BlueprintFixtureSource::GetMainClass();
    if (Class == nullptr)
    {
        AddError(TEXT("The main fixture class is missing."));
        return false;
    }

    UObject* const CDO = Class->GetDefaultObject();
    FIntProperty* const IntProperty = FindFProperty<FIntProperty>(Class, TEXT("IntVar"));
    if (CDO == nullptr || IntProperty == nullptr)
    {
        AddError(TEXT("The fixture class has no IntVar."));
        return false;
    }

    const int32 Original = IntProperty->GetPropertyValue_InContainer(CDO);
    ON_SCOPE_EXIT
    {
        IntProperty->SetPropertyValue_InContainer(CDO, Original);
    };
    IntProperty->SetPropertyValue_InContainer(CDO, 123);

    const FBlueprintExpressionContext Context{CDO, nullptr, nullptr};
    FString Error;

    FBlueprintExpressionValue Value;
    TestTrue(TEXT("This.IntVar resolves"), BlueprintExpression::EvaluateValue(Context, TEXT("This.IntVar"), Value, Error));
    TestEqual(TEXT("the display value is 123"), Value.Display, FString(TEXT("123")));
    TestTrue(TEXT("the value is numeric"), Value.bIsNumeric);

    Value = FBlueprintExpressionValue();
    TestTrue(TEXT("a bare property resolves"), BlueprintExpression::EvaluateValue(Context, TEXT("IntVar"), Value, Error));
    TestTrue(TEXT("the bare property is numeric"), Value.bIsNumeric);
    TestEqual(TEXT("the bare property value is 123"), Value.Display, FString(TEXT("123")));

    bool bResult = false;
    TestTrue(TEXT("equality parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("This.IntVar == 123"), bResult, Error));
    TestTrue(TEXT("equality is true"), bResult);
    TestTrue(TEXT("a successful comparison reports no error"), Error.IsEmpty());
    TestTrue(TEXT("an ordered comparison parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("This.IntVar > 100"), bResult, Error));
    TestTrue(TEXT("ordered comparison is true"), bResult);
    TestTrue(TEXT("a false comparison parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("This.IntVar <= 100"), bResult, Error));
    TestFalse(TEXT("the comparison is false"), bResult);
    TestTrue(TEXT("negation parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("!This.IntVar == 100"), bResult, Error));
    TestTrue(TEXT("negation is true"), bResult);
    TestTrue(TEXT("negation of a true comparison parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("!This.IntVar == 123"), bResult, Error));
    TestFalse(TEXT("negation of a true comparison is false"), bResult);
    TestTrue(TEXT("a successful negation reports no error"), Error.IsEmpty());

    TestTrue(TEXT("an inequality parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("This.IntVar != 100"), bResult, Error));
    TestTrue(TEXT("the inequality is true"), bResult);
    TestTrue(TEXT("an inequality of equal values parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("This.IntVar != 123"), bResult, Error));
    TestFalse(TEXT("an inequality of equal values is false"), bResult);
    TestTrue(TEXT("a successful inequality reports no error"), Error.IsEmpty());

    TestTrue(TEXT("a true literal parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("true"), bResult, Error) && bResult);
    TestTrue(TEXT("a false literal parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("false"), bResult, Error) && !bResult);
    TestTrue(TEXT("a negated true literal is false"), BlueprintExpression::EvaluateCondition(Context, TEXT("!true"), bResult, Error) && !bResult);
    TestTrue(TEXT("a negated empty string is true"), BlueprintExpression::EvaluateCondition(Context, TEXT("!\"\""), bResult, Error) && bResult);
    TestTrue(TEXT("a negated null is true"), BlueprintExpression::EvaluateCondition(Context, TEXT("!null"), bResult, Error) && bResult);
    TestTrue(TEXT("a negated zero is true"), BlueprintExpression::EvaluateCondition(Context, TEXT("!0"), bResult, Error) && bResult);
    TestTrue(TEXT("a literal left operand parses"), BlueprintExpression::EvaluateCondition(Context, TEXT("123 == This.IntVar"), bResult, Error));
    TestTrue(TEXT("the literal left operand compares true"), bResult);
    TestTrue(TEXT("two text literals compare"), BlueprintExpression::EvaluateCondition(Context, TEXT("\"a\" == \"a\""), bResult, Error) && bResult);
    TestTrue(TEXT("two different text literals are unequal"), BlueprintExpression::EvaluateCondition(Context, TEXT("\"a\" != \"b\""), bResult, Error) && bResult);

    Value = FBlueprintExpressionValue();
    TestTrue(TEXT("a negated literal evaluates as a value"), BlueprintExpression::EvaluateValue(Context, TEXT("!true"), Value, Error));
    TestEqual(TEXT("the negated literal displays false"), Value.Display, FString(TEXT("false")));

    Error.Reset();
    Value = FBlueprintExpressionValue();
    TestFalse(TEXT("a missing property fails"), BlueprintExpression::EvaluateValue(Context, TEXT("This.NoSuchProperty"), Value, Error));
    TestTrue(TEXT("the missing-property error is reported"), !Error.IsEmpty());

    const FString Log = BlueprintExpression::FormatLogMessage(Context, TEXT("IntVar={This.IntVar}"));
    TestEqual(TEXT("the logpoint substitutes the value"), Log, FString(TEXT("IntVar=123")));
    TestEqual(TEXT("a doubled pair is not a placeholder"), BlueprintExpression::FormatLogMessage(Context, TEXT("{{IntVar}}")), FString(TEXT("{IntVar}")));
    TestEqual(TEXT("doubled braces keep a JSON-looking message literal"), BlueprintExpression::FormatLogMessage(Context, TEXT("{{\"a\": 1}}")), FString(TEXT("{\"a\": 1}")));
    TestEqual(TEXT("a quoted close brace stays inside the placeholder"), BlueprintExpression::FormatLogMessage(Context, TEXT("{This.StringVar == \"}\"}")), FString(TEXT("false")));

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintExpressionHitTest, "BlueprintInspector.Expression.HitConditions", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintExpressionHitTest::RunTest(const FString& Parameters)
{
    bool bBreak = false;

    TestTrue(TEXT("a bare count parses"), BlueprintExpression::EvaluateHitCondition(TEXT("5"), 4, bBreak));
    TestFalse(TEXT("the fourth hit does not break"), bBreak);
    TestTrue(TEXT("the fifth hit breaks"), BlueprintExpression::EvaluateHitCondition(TEXT("5"), 5, bBreak) && bBreak);
    TestTrue(TEXT(">= parses"), BlueprintExpression::EvaluateHitCondition(TEXT(">=5"), 5, bBreak) && bBreak);
    TestTrue(TEXT("< parses"), BlueprintExpression::EvaluateHitCondition(TEXT("<5"), 4, bBreak) && bBreak);
    TestTrue(TEXT("<= parses"), BlueprintExpression::EvaluateHitCondition(TEXT("<=5"), 5, bBreak) && bBreak);
    TestTrue(TEXT("> parses"), BlueprintExpression::EvaluateHitCondition(TEXT(">5"), 6, bBreak) && bBreak);
    TestTrue(TEXT("== parses"), BlueprintExpression::EvaluateHitCondition(TEXT("==5"), 5, bBreak) && bBreak);
    TestTrue(TEXT("== parses for other hits"), BlueprintExpression::EvaluateHitCondition(TEXT("==5"), 6, bBreak));
    TestFalse(TEXT("== does not break other hits"), bBreak);
    TestTrue(TEXT("modulo parses"), BlueprintExpression::EvaluateHitCondition(TEXT("%5"), 10, bBreak) && bBreak);
    TestTrue(TEXT("modulo skips other hits"), BlueprintExpression::EvaluateHitCondition(TEXT("%5"), 11, bBreak) && !bBreak);
    bBreak = true;
    TestFalse(TEXT("a zero modulo target is rejected"), BlueprintExpression::EvaluateHitCondition(TEXT("%0"), 5, bBreak));
    TestFalse(TEXT("a failed modulo parse does not request a break"), bBreak);
    TestTrue(TEXT("a target beyond int32 parses"), BlueprintExpression::EvaluateHitCondition(TEXT(">=3000000000"), 5, bBreak));
    TestFalse(TEXT("a hit count below a wide target does not break"), bBreak);
    TestTrue(TEXT("a wide modulo parses"), BlueprintExpression::EvaluateHitCondition(TEXT("%2147483648"), 2147483648LL, bBreak));
    TestTrue(TEXT("the wide modulo breaks on its multiple"), bBreak);
    bBreak = true;
    TestFalse(TEXT("garbage fails"), BlueprintExpression::EvaluateHitCondition(TEXT("nope"), 5, bBreak));
    TestFalse(TEXT("a failed parse does not request a break"), bBreak);
    TestTrue(TEXT("an empty condition always breaks"), BlueprintExpression::EvaluateHitCondition(FString(), 1, bBreak) && bBreak);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintExpressionUnsignedTest, "BlueprintInspector.Expression.UnsignedInteger", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintExpressionUnsignedTest::RunTest(const FString& Parameters)
{
    UOpcodeProbeHost* const Host = NewObject<UOpcodeProbeHost>();
    TStrongObjectPtr<UOpcodeProbeHost> HostGuard(Host);
    if (!TestNotNull(TEXT("probe host"), Host))
    {
        return false;
    }

    Host->UInt64Var = uint64(1) << 63;

    const FBlueprintExpressionContext Context{Host, nullptr, nullptr};
    FString Error;
    FBlueprintExpressionValue Value;
    TestTrue(TEXT("This.UInt64Var resolves"), BlueprintExpression::EvaluateValue(Context, TEXT("This.UInt64Var"), Value, Error));
    TestTrue(TEXT("the high bit is not read as a sign"), Value.Number > 0.0);
    TestEqual(TEXT("the unsigned value is exact"), Value.Number, 9223372036854775808.0);
    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
