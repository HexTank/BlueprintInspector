// Copyright (c) 2024-2026 Paul Tankard

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

#include "BlueprintInspectorPort.h"
#include "UObject/Package.h"
#include "UObject/UnrealType.h"
#if BI_ENGINE_AT_LEAST_5_5
#include "UObject/StrProperty.h"
#endif

#include "BlueprintAudit.h"
#include "BlueprintFixtureSource.h"

namespace BlueprintAuditTests
{
/** How many BPI010 default-value errors the scan reported for this path (a complete path or a prefix). */
int32 CountDefaultValueFindings(const FBlueprintAuditResult& Result, const FString& Path)
{
    const FString Expected = FString::Printf(TEXT("default value '%s"), *Path);
    int32 Count = 0;
    for (const FBlueprintAuditFinding& Finding : Result.Findings)
    {
        if (Finding.Rule == TEXT("BPI010") && Finding.Severity == EBlueprintAuditSeverity::Error && Finding.Message.Contains(Expected))
        {
            ++Count;
        }
    }
    return Count;
}
} // namespace BlueprintAuditTests

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAuditSecretTest, "BlueprintInspector.Audit.SecretPatterns", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAuditSecretTest::RunTest(const FString& Parameters)
{
    // Every pattern is pinned on both sides: a value that must be caught and its pattern's name, then a
    // value that must not be caught. A reordering of the checks in SecretPatternName or a loosened regex
    // has to fail here, because the audit's whole security value rests on these six rules.
    struct FCase
    {
        const TCHAR* Value;
        const TCHAR* Pattern;
    };

    const FCase Positives[] = {
        {TEXT("api_key=abc123"), TEXT("key/value assignment")},
        {TEXT("AKIAIOSFODNN7EXAMPLE"), TEXT("AWS access key")},
        {TEXT("eyJhbGciOiJIUzI1NiJ9.eyJzdWIiOiIxMjM0NTY3ODkwIn0.dozjgNryP4J3jVmNHl0w5N_XgL0n3I9PlFUP0THsR8U"), TEXT("JWT")},
        {TEXT("postgres://user:pass@localhost/db"), TEXT("URL with credentials")},
        {TEXT("https://user:pass@host/path"), TEXT("URL with credentials")},
        {TEXT("-----BEGIN RSA PRIVATE KEY-----"), TEXT("private key")},
        {TEXT("sk-abcdefghijklmnopqrstuvwx"), TEXT("API key")},
        {TEXT("mysql://user:pass@host/db?api_key=abc"), TEXT("URL with credentials")},
        {TEXT("secret=sk-abcdefghijklmnopqrstuvwx"), TEXT("API key")},
    };

    for (const FCase& Case : Positives)
    {
        TestTrue(*FString::Printf(TEXT("%s is a secret pattern"), Case.Value), BlueprintAudit::ContainsSecretPattern(Case.Value));
        TestEqual(*FString::Printf(TEXT("%s is named"), Case.Value), BlueprintAudit::SecretPatternName(Case.Value), FString(Case.Pattern));
    }

    const TCHAR* const Negatives[] = {
        TEXT("https://example.com/path"),
        TEXT("GameplayCue Burst Executed"),
        TEXT("password label"),
        TEXT("sk-short"),
        TEXT(""),
    };

    for (const TCHAR* const Negative : Negatives)
    {
        TestFalse(*FString::Printf(TEXT("'%s' is not a secret"), Negative), BlueprintAudit::ContainsSecretPattern(Negative));
        TestTrue(*FString::Printf(TEXT("'%s' has no pattern name"), Negative), BlueprintAudit::SecretPatternName(Negative).IsEmpty());
    }

    // The mask format is pinned exactly, and it must not carry the value or any part of it: the mask is
    // what ships to the DAP client as evidence.
    struct FMaskCase
    {
        const TCHAR* Value;
        const TCHAR* Expected;
    };

    const FMaskCase Masks[] = {
        {TEXT("s3cr3t-value"), TEXT("<redacted: 12 char(s)>")},
        {TEXT("api_key=abc123"), TEXT("<redacted: 14 char(s)>")},
        {TEXT("AKIAIOSFODNN7EXAMPLE"), TEXT("<redacted: 20 char(s)>")},
    };

    for (const FMaskCase& Case : Masks)
    {
        const FString Masked = BlueprintAudit::MaskSecret(Case.Value);
        TestEqual(*FString::Printf(TEXT("the mask of '%s' is stable"), Case.Value), Masked, FString(Case.Expected));
        TestFalse(*FString::Printf(TEXT("the mask of '%s' does not leak the value"), Case.Value), Masked.Contains(Case.Value));
    }

    return true;
}

// A class can hold a credential anywhere a default value can: a plain string, an array element, a set
// element, a map value, a struct member or a soft reference's path. This test pins the coverage and the
// path each finding names.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAuditDefaultValueCoverageTest, "BlueprintInspector.Audit.DefaultValueCoverage", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAuditDefaultValueCoverageTest::RunTest(const FString& Parameters)
{
    UClass* const Class = BlueprintFixtureSource::GetMainClass();
    if (!TestNotNull(TEXT("the fixture class exists"), Class))
    {
        return false;
    }

    UObject* const CDO = Class->GetDefaultObject();
    if (!TestNotNull(TEXT("the fixture has a default object"), CDO))
    {
        return false;
    }

    FStrProperty* const StringProperty = FindFProperty<FStrProperty>(Class, TEXT("StringVar"));
    FArrayProperty* const StringArrayProperty = FindFProperty<FArrayProperty>(Class, TEXT("StringArrayVar"));
    FSetProperty* const SetProperty = FindFProperty<FSetProperty>(Class, TEXT("SetVar"));
    FMapProperty* const MapProperty = FindFProperty<FMapProperty>(Class, TEXT("MapVar"));
    FStructProperty* const StructProperty = FindFProperty<FStructProperty>(Class, TEXT("StructVar"));
    FSoftObjectProperty* const SoftProperty = FindFProperty<FSoftObjectProperty>(Class, TEXT("SoftObjVar"));
    bool bHasProperties = true;
    bHasProperties &= TestTrue(TEXT("the fixture has StringVar"), StringProperty != nullptr);
    bHasProperties &= TestTrue(TEXT("the fixture has StringArrayVar"), StringArrayProperty != nullptr);
    bHasProperties &= TestTrue(TEXT("the fixture has SetVar"), SetProperty != nullptr);
    bHasProperties &= TestTrue(TEXT("the fixture has MapVar"), MapProperty != nullptr);
    bHasProperties &= TestTrue(TEXT("the fixture has StructVar"), StructProperty != nullptr);
    bHasProperties &= TestTrue(TEXT("the fixture has SoftObjVar"), SoftProperty != nullptr);
    if (!bHasProperties)
    {
        return false;
    }

    const bool bStringArrayShape = TestTrue(TEXT("StringArrayVar holds strings"), StringArrayProperty && CastField<FStrProperty>(StringArrayProperty->Inner) != nullptr);
    const bool bSetShape = TestTrue(TEXT("SetVar holds names"), SetProperty && CastField<FNameProperty>(SetProperty->ElementProp) != nullptr);
    const bool bMapShape = TestTrue(TEXT("MapVar maps int to name"), MapProperty && CastField<FIntProperty>(MapProperty->KeyProp) != nullptr && CastField<FNameProperty>(MapProperty->ValueProp) != nullptr);
    const bool bStructShape = TestTrue(TEXT("StructVar holds a struct"), StructProperty && StructProperty->Struct != nullptr);
    if (!bStringArrayShape || !bSetShape || !bMapShape || !bStructShape)
    {
        return false;
    }

    // A user-defined struct names its members with a generated suffix, so the string member is found by
    // type rather than by the authored name; S_ProbeStruct has exactly one.
    FStrProperty* StructStringMember = nullptr;
    for (TFieldIterator<FProperty> MemberIt(StructProperty->Struct); MemberIt; ++MemberIt)
    {
        if (FStrProperty* const Candidate = CastField<FStrProperty>(*MemberIt))
        {
            StructStringMember = Candidate;
            break;
        }
    }
    if (!TestNotNull(TEXT("the fixture struct has a string member"), StructStringMember))
    {
        return false;
    }

    UPackage* const ClassPackage = Class->GetOutermost();
    if (ClassPackage == nullptr)
    {
        AddError(TEXT("The fixture class has no package."));
        return false;
    }

    // These defaults are shared state: put every one back when the test ends, whatever it does.
    const FString OldString = StringProperty->GetPropertyValue_InContainer(CDO);
    TArray<FString>& StringArray = *StringArrayProperty->ContainerPtrToValuePtr<TArray<FString>>(CDO);
    const TArray<FString> OldStringArray = StringArray;
    TSet<FName>& Names = *SetProperty->ContainerPtrToValuePtr<TSet<FName>>(CDO);
    const TSet<FName> OldNames = Names;
    TMap<int32, FName>& Lookup = *MapProperty->ContainerPtrToValuePtr<TMap<int32, FName>>(CDO);
    const TMap<int32, FName> OldLookup = Lookup;
    void* const StructValue = StructProperty->ContainerPtrToValuePtr<void>(CDO);
    const FString OldStructString = StructStringMember->GetPropertyValue(StructStringMember->ContainerPtrToValuePtr<void>(StructValue));
    FSoftObjectPtr& SoftValue = *SoftProperty->ContainerPtrToValuePtr<FSoftObjectPtr>(CDO);
    const FSoftObjectPtr OldSoftValue = SoftValue;
    const bool bWasPackageDirty = ClassPackage->IsDirty();
    ON_SCOPE_EXIT
    {
        StringProperty->SetPropertyValue_InContainer(CDO, OldString);
        StringArray = OldStringArray;
        Names = OldNames;
        Lookup = OldLookup;
        StructStringMember->SetPropertyValue(StructStringMember->ContainerPtrToValuePtr<void>(StructValue), OldStructString);
        SoftValue = OldSoftValue;
        ClassPackage->SetDirtyFlag(bWasPackageDirty);
    };

    StringProperty->SetPropertyValue_InContainer(CDO, TEXT("api_key=abc123"));
    StringArray.Reset();
    StringArray.Add(TEXT("plain"));
    StringArray.Add(TEXT("api_key=abc123"));
    Names.Reset();
    Names.Add(FName(TEXT("AKIAIOSFODNN7EXAMPLE")));
    Lookup.Reset();
    Lookup.Add(0, FName(TEXT("sk-abcdefghijklmnopqrstuvwx")));
    StructStringMember->SetPropertyValue(StructStringMember->ContainerPtrToValuePtr<void>(StructValue), TEXT("-----BEGIN RSA PRIVATE KEY-----"));
    // A soft path has to look like a package path to be stored at all (FSoftObjectPath::SetPath rejects
    // anything else), so this one carries the credential in the assignment shape a path can hold.
    SoftValue = FSoftObjectPtr(FSoftObjectPath(TEXT("/Game/Secrets/api_key=abc123.api_key=abc123")));

    TArray<UObject*> Classes;
    Classes.Add(Class);
    const FBlueprintAuditResult Result = BlueprintAudit::Run(Classes, 500);

    TestTrue(TEXT("the fixture class was scanned"), Result.ClassesScanned >= 1);
    TestEqual(TEXT("a plain string default is one finding"), BlueprintAuditTests::CountDefaultValueFindings(Result, TEXT("StringVar")), 1);
    TestEqual(TEXT("an array element is named by its index"), BlueprintAuditTests::CountDefaultValueFindings(Result, TEXT("StringArrayVar[1]")), 1);
    TestEqual(TEXT("a set element is inspected"), BlueprintAuditTests::CountDefaultValueFindings(Result, TEXT("SetVar[")), 1);
    TestEqual(TEXT("a map value is inspected"), BlueprintAuditTests::CountDefaultValueFindings(Result, TEXT("MapVar[0].Value")), 1);
    TestEqual(TEXT("a plain array element is not reported"), BlueprintAuditTests::CountDefaultValueFindings(Result, TEXT("StringArrayVar[0]")), 0);
    TestEqual(TEXT("a struct member is inspected"), BlueprintAuditTests::CountDefaultValueFindings(Result, FString::Printf(TEXT("StructVar.%s"), *StructStringMember->GetName())), 1);
    TestEqual(TEXT("a soft reference path is inspected"), BlueprintAuditTests::CountDefaultValueFindings(Result, TEXT("SoftObjVar")), 1);

    // Evidence reaches the DAP client, so no path may ship the value it names.
    for (const FBlueprintAuditFinding& Finding : Result.Findings)
    {
        if (Finding.Rule != TEXT("BPI010"))
        {
            continue;
        }

        const TArray<FString> Shipped = {Finding.Message, Finding.FunctionName, Finding.Evidence.ToString()};
        for (const FString& Text : Shipped)
        {
            TestFalse(TEXT("no field carries a key/value secret"), Text.Contains(TEXT("abc123")));
            TestFalse(TEXT("no field carries an AWS key"), Text.Contains(TEXT("AKIA")));
            TestFalse(TEXT("no field carries a Stripe key"), Text.Contains(TEXT("sk-abc")));
            TestFalse(TEXT("no field carries a private key body"), Text.Contains(TEXT("-----BEGIN")));
            TestFalse(TEXT("no field carries a private key marker"), Text.Contains(TEXT("PRIVATE KEY-----")));
            TestFalse(TEXT("no field carries URL credentials"), Text.Contains(TEXT("user:pass")));
        }
        TestTrue(TEXT("evidence is redacted"), Finding.Evidence.ToString().Contains(TEXT("redacted")));
    }

    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAuditEvidenceRedactionTest, "BlueprintInspector.Audit.EvidenceRedactionSpans", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAuditEvidenceRedactionTest::RunTest(const FString& Parameters)
{
    const FString Overlapping = TEXT("Log(\"api_key=abc\", \"api_key=abcdef\");");
    const FString Redacted = FRedactedText::RedactedLine(Overlapping).ToString();
    TestFalse(TEXT("the shorter secret is not shipped"), Redacted.Contains(TEXT("api_key=abc")));
    TestFalse(TEXT("the longer literal's tail is not shipped"), Redacted.Contains(TEXT("abcdef")));

    int32 Masks = 0;
    int32 From = 0;
    while (true)
    {
        const int32 Found = Redacted.Find(TEXT("<redacted:"), ESearchCase::CaseSensitive, ESearchDir::FromStart, From);
        if (Found == INDEX_NONE)
        {
            break;
        }
        ++Masks;
        From = Found + FCString::Strlen(TEXT("<redacted:"));
    }
    TestEqual(TEXT("each overlapping literal is masked"), Masks, 2);

    const FString Repeated = TEXT("Log(\"api_key=abc\", api_key=abc);");
    const FString RepeatedRedacted = FRedactedText::RedactedLine(Repeated).ToString();
    TestFalse(TEXT("an unquoted credential does not survive"), RepeatedRedacted.Contains(TEXT("api_key=abc")));
    TestTrue(TEXT("a residual match masks the whole line"), RepeatedRedacted.Contains(TEXT("<redacted:")));

    const FString BareUrl = TEXT("Log(mysql://user:pass@host/db);");
    const FString BareUrlRedacted = FRedactedText::RedactedLine(BareUrl).ToString();
    TestFalse(TEXT("bare URL credentials do not ship"), BareUrlRedacted.Contains(TEXT("user:pass")));
    TestFalse(TEXT("the bare URL host does not ship"), BareUrlRedacted.Contains(TEXT("host")));

    const FString BareAws = TEXT("Log(AKIAIOSFODNN7EXAMPLE);");
    const FString BareAwsRedacted = FRedactedText::RedactedLine(BareAws).ToString();
    TestFalse(TEXT("a bare AWS key does not ship"), BareAwsRedacted.Contains(TEXT("AKIA")));

    const FString Unterminated = TEXT("Log(\"api_key=abc);");
    const FString UnterminatedRedacted = FRedactedText::RedactedLine(Unterminated).ToString();
    TestFalse(TEXT("an unterminated literal's secret does not ship"), UnterminatedRedacted.Contains(TEXT("api_key=abc")));
    TestTrue(TEXT("the unterminated tail is masked"), UnterminatedRedacted.Contains(TEXT("<redacted:")));

    const FString CutUrl = FString::ChrN(180, TEXT('x')) + TEXT(" https://user:secretpw@host/path");
    const FString CutUrlRedacted = FRedactedText::RedactedLine(CutUrl).ToString();
    TestFalse(TEXT("a credential cut by the evidence limit does not ship"), CutUrlRedacted.Contains(TEXT("user:secre")));
    TestTrue(TEXT("the cut credential masks the whole line"), CutUrlRedacted.StartsWith(TEXT("<redacted:")));
    return true;
}

// BPI031 names the cast that is guarded. An unguarded cast beside a guarded one is reportable, including
// a class cast, and these cases pin both directions without an authored fixture graph.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAuditCastGuardTest, "BlueprintInspector.Audit.CastGuard", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAuditCastGuardTest::RunTest(const FString& Parameters)
{
    const auto Check = [this](const TCHAR* Name, const TArray<FString>& Lines, const TArray<FString>& ExpectedVariables) {
        const TArray<BlueprintAudit::FUncheckedCast> Found = BlueprintAudit::FindUncheckedCasts(Lines);

        TArray<FString> Variables;
        for (const BlueprintAudit::FUncheckedCast& Cast : Found)
        {
            Variables.Add(Cast.Variable);

            // The finding has to point at the cast, so the line number must be that statement's own
            // assignment, not merely a line that mentions the variable next to some other cast.
            const FString Assignment = FString::Printf(TEXT("%s = Cast<"), *Cast.Variable);
            const bool bLineNamesCast = Lines.IsValidIndex(Cast.Line - 1) && Lines[Cast.Line - 1].Contains(Assignment);
            TestTrue(*FString::Printf(TEXT("%s: '%s' is reported at its own cast line"), Name, *Cast.Variable), bLineNamesCast);
        }

        TestEqual(*FString::Printf(TEXT("%s: unchecked casts"), Name), FString::Join(Variables, TEXT(", ")), FString::Join(ExpectedVariables, TEXT(", ")));
    };

    // Two casts, one guard: the old totals saw 2 casts and 3 success mentions and stayed silent, so the
    // second cast went unreported.
    Check(TEXT("a guarded cast does not hide an unguarded one"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tif (K2Node_DynamicCast_bSuccess)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}"),
           TEXT("\tK2Node_DynamicCast_1_AsBPI_Probe_A = Cast<IBPI_ProbeA_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_1_bSuccess = K2Node_DynamicCast_1_AsBPI_Probe_A != nullptr;"),
           TEXT("\tInterfaceVar = K2Node_DynamicCast_1_AsBPI_Probe_A;")},
          {TEXT("K2Node_DynamicCast_1_AsBPI_Probe_A")});

    // A guard that reads the other cast's flag does not cover this one.
    Check(TEXT("a guard for another cast is not a guard"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tK2Node_DynamicCast_1_AsBPI_Probe_A = Cast<IBPI_ProbeA_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_1_bSuccess = K2Node_DynamicCast_1_AsBPI_Probe_A != nullptr;"),
           TEXT("\tif (K2Node_DynamicCast_bSuccess)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}"),
           TEXT("\tInterfaceVar = K2Node_DynamicCast_1_AsBPI_Probe_A;")},
          {TEXT("K2Node_DynamicCast_1_AsBPI_Probe_A")});

    // The negated form is the common one in a graph, and it is still a check.
    Check(TEXT("a negated guard counts"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tif (!K2Node_DynamicCast_bSuccess)"),
           TEXT("\t{"),
           TEXT("\t\treturn;"),
           TEXT("\t}"),
           TEXT("\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;")},
          {});

    // A class cast is a dynamic cast too; the old prefix matched only the plain temp.
    Check(TEXT("a class cast is reported"),
          {TEXT("\tK2Node_ClassDynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ClassVar);"),
           TEXT("\tK2Node_ClassDynamicCast_bSuccess = K2Node_ClassDynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tClassVar = K2Node_ClassDynamicCast_AsBP_Probe_Cast_Target;")},
          {TEXT("K2Node_ClassDynamicCast_AsBP_Probe_Cast_Target")});

    // IsValid on the result, branched on through its own temp, is the check the rule wants to see.
    Check(TEXT("an IsValid copy counts as the check"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tK2Node_CallFunction_IsValid = IsValid(K2Node_DynamicCast_AsBP_Probe_Cast_Target);"),
           TEXT("\tif (K2Node_CallFunction_IsValid)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}")},
          {});

    // An inverted flag is the failure path, so an access under it is unguarded.
    Check(TEXT("a Not_PreBool branch is the failure path"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tK2Node_CallFunction_Not = Not_PreBool(K2Node_DynamicCast_bSuccess);"),
           TEXT("\tif (K2Node_CallFunction_Not)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}")},
          {TEXT("K2Node_DynamicCast_AsBP_Probe_Cast_Target")});

    // A negated condition guards the failure branch, not an access inside it.
    Check(TEXT("a failure-branch negation is not a guard"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tif (!K2Node_DynamicCast_bSuccess)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}")},
          {TEXT("K2Node_DynamicCast_AsBP_Probe_Cast_Target")});

    // A disjunction with an unrelated operand does not imply the cast succeeded.
    Check(TEXT("a disjunction with another operand is not a guard"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tif (K2Node_DynamicCast_bSuccess || OtherVar)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}")},
          {TEXT("K2Node_DynamicCast_AsBP_Probe_Cast_Target")});

    // A conjunction does imply it.
    Check(TEXT("a conjunction is a guard"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tif (K2Node_DynamicCast_bSuccess && OtherVar)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}")},
          {});

    // Contradictory guards admit no execution path, so they cannot cover the access.
    Check(TEXT("contradictory guards do not hide the cast"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tif (K2Node_DynamicCast_bSuccess)"),
           TEXT("\t{"),
           TEXT("\t\tif (!K2Node_DynamicCast_bSuccess)"),
           TEXT("\t\t{"),
           TEXT("\t\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t\t}"),
           TEXT("\t}")},
          {TEXT("K2Node_DynamicCast_AsBP_Probe_Cast_Target")});

    // Both disjuncts derived from the same cast imply it.
    Check(TEXT("a disjunction of two views of the same cast is a guard"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tK2Node_CallFunction_IsValid = IsValid(K2Node_DynamicCast_AsBP_Probe_Cast_Target);"),
           TEXT("\tif (K2Node_DynamicCast_bSuccess || K2Node_CallFunction_IsValid)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}")},
          {});

    // The else exits, so the code after the if/else is the success path.
    Check(TEXT("an exiting else guards the fallthrough"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tif (K2Node_DynamicCast_bSuccess)"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\t}"),
           TEXT("\telse"),
           TEXT("\t{"),
           TEXT("\t\treturn;"),
           TEXT("\t}"),
           TEXT("\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;")},
          {});

    // Copying the result is not a test of it; only a condition or a derived flag is.
    Check(TEXT("a plain copy of the result is not a check"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;")},
          {TEXT("K2Node_DynamicCast_AsBP_Probe_Cast_Target")});

    // Two unchecked casts are two findings; the old code reported at most one per function, and only
    // when the totals happened to cross.
    Check(TEXT("every unchecked cast is reported"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tObjectVar = K2Node_DynamicCast_AsBP_Probe_Cast_Target;"),
           TEXT("\tK2Node_DynamicCast_1_AsBPI_Probe_A = Cast<IBPI_ProbeA_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_1_bSuccess = K2Node_DynamicCast_1_AsBPI_Probe_A != nullptr;"),
           TEXT("\tInterfaceVar = K2Node_DynamicCast_1_AsBPI_Probe_A;")},
          {TEXT("K2Node_DynamicCast_AsBP_Probe_Cast_Target"), TEXT("K2Node_DynamicCast_1_AsBPI_Probe_A")});

    // A cast whose result is never read needs no null guard; the checked flag alone must not turn it
    // into a finding.
    Check(TEXT("an unused cast with a checked flag is not reported"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tK2Node_DynamicCast_bSuccess = K2Node_DynamicCast_AsBP_Probe_Cast_Target != nullptr;"),
           TEXT("\tif (!K2Node_DynamicCast_bSuccess)"),
           TEXT("\t{"),
           TEXT("\t\tUKismetSystemLibrary::LogString(\"invalid type\", true);"),
           TEXT("\t}"),
           TEXT("\tReturnValue = nullptr;")},
          {});

    // A cast with no flag at all and no read is dead, not unchecked.
    Check(TEXT("an unused cast with no flag is not reported"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);")},
          {});

    // A read inside the condition is the check itself, not an unguarded access.
    Check(TEXT("a condition-only use is a guard"),
          {TEXT("\tK2Node_DynamicCast_AsBP_Probe_Cast_Target = Cast<ABP_ProbeCastTarget_C>(ActorVar);"),
           TEXT("\tif (IsValid(K2Node_DynamicCast_AsBP_Probe_Cast_Target))"),
           TEXT("\t{"),
           TEXT("\t\tObjectVar = OtherVar;"),
           TEXT("\t}")},
          {});

    // An implicit numeric conversion renders through the same keyword; it cannot be null and is not a
    // dynamic cast, so the widened match must not pick it up.
    Check(TEXT("an implicit numeric conversion is not a dynamic cast"),
          {TEXT("\tK2Node_VariableSet_RealVar_ImplicitCast = Cast<double>(CallFunc_GetSinglePrecision_ReturnValue);"),
           TEXT("\tRealVar = K2Node_VariableSet_RealVar_ImplicitCast;")},
          {});

    return true;
}

// The duplicate-name group shares class, function and line, so the report order must not depend on the
// order the packages came out of a TSet. The message and the sort order are pinned here: both must be
// total, or two runs of the same project can disagree.
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FBlueprintAuditReportOrderTest, "BlueprintInspector.Audit.ReportOrderIsDeterministic", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FBlueprintAuditReportOrderTest::RunTest(const FString& Parameters)
{
    const TArray<FString> Packages = {TEXT("/Game/Zeta"), TEXT("/Game/Alpha"), TEXT("/Game/Delta"), TEXT("/Game/Alpha"), TEXT("/Game/Gamma"), TEXT("/Game/Beta"), TEXT("/Game/Epsilon")};
    TestEqual(TEXT("the package list is sorted, de-duplicated and capped"), BlueprintAudit::FormatPackageList(Packages, 5), FString(TEXT("/Game/Alpha, /Game/Beta, /Game/Delta, /Game/Epsilon, /Game/Gamma, ...")));
    TestEqual(TEXT("a zero cap shows nothing"), BlueprintAudit::FormatPackageList(Packages, 0), FString());
    TestEqual(TEXT("a negative cap shows nothing"), BlueprintAudit::FormatPackageList(Packages, -1), FString());
    TestEqual(TEXT("a cap at the unique count shows every entry without an ellipsis"), BlueprintAudit::FormatPackageList(Packages, 6), FString(TEXT("/Game/Alpha, /Game/Beta, /Game/Delta, /Game/Epsilon, /Game/Gamma, /Game/Zeta")));

    const auto MakeFinding = [](const TCHAR* PackageName) {
        FBlueprintAuditFinding Finding;
        Finding.Rule = TEXT("BPI040");
        Finding.Severity = EBlueprintAuditSeverity::Warning;
        Finding.Category = TEXT("Hygiene");
        Finding.ClassName = TEXT("BP_Shared");
        Finding.Line = 0;
        Finding.Message = FString::Printf(TEXT("Class short name is used by 2 packages (%s)."), PackageName);
        Finding.Evidence = FRedactedText::Authored(PackageName);
        return Finding;
    };

    TArray<FBlueprintAuditFinding> Findings;
    Findings.Add(MakeFinding(TEXT("/Game/Zeta")));
    Findings.Add(MakeFinding(TEXT("/Game/Alpha")));
    BlueprintAudit::SortFindings(Findings);
    if (!TestEqual(TEXT("both findings are present"), Findings.Num(), 2))
    {
        return false;
    }
    TestEqual(TEXT("the first finding is the alphabetically first package"), Findings[0].Evidence.ToString(), FString(TEXT("/Game/Alpha")));
    TestEqual(TEXT("the second finding is the alphabetically last package"), Findings[1].Evidence.ToString(), FString(TEXT("/Game/Zeta")));

    TArray<FBlueprintAuditFinding> Reversed;
    Reversed.Add(MakeFinding(TEXT("/Game/Alpha")));
    Reversed.Add(MakeFinding(TEXT("/Game/Zeta")));
    BlueprintAudit::SortFindings(Reversed);
    if (!TestEqual(TEXT("both reversed findings are present"), Reversed.Num(), 2))
    {
        return false;
    }
    TestEqual(TEXT("both input orders sort to the same first finding"), Reversed[0].Evidence.ToString(), Findings[0].Evidence.ToString());
    TestEqual(TEXT("both input orders sort to the same second finding"), Reversed[1].Evidence.ToString(), Findings[1].Evidence.ToString());

    const auto MakeTieFinding = [](const TCHAR* Message, const TCHAR* Evidence) {
        FBlueprintAuditFinding Finding;
        Finding.Rule = TEXT("BPI040");
        Finding.Severity = EBlueprintAuditSeverity::Warning;
        Finding.Category = TEXT("Hygiene");
        Finding.ClassName = TEXT("BP_Shared");
        Finding.Line = 0;
        Finding.Message = Message;
        Finding.Evidence = FRedactedText::Authored(Evidence);
        return Finding;
    };

    TArray<FBlueprintAuditFinding> SameMessage;
    SameMessage.Add(MakeTieFinding(TEXT("Class short name is used by 2 packages."), TEXT("/Game/Zeta")));
    SameMessage.Add(MakeTieFinding(TEXT("Class short name is used by 2 packages."), TEXT("/Game/Alpha")));
    BlueprintAudit::SortFindings(SameMessage);
    if (!TestEqual(TEXT("both same-message findings are present"), SameMessage.Num(), 2))
    {
        return false;
    }
    TestEqual(TEXT("the evidence breaks a message tie"), SameMessage[0].Evidence.ToString(), FString(TEXT("/Game/Alpha")));

    TArray<FBlueprintAuditFinding> SameEvidence;
    SameEvidence.Add(MakeTieFinding(TEXT("Zeta message."), TEXT("/Game/Shared")));
    SameEvidence.Add(MakeTieFinding(TEXT("Alpha message."), TEXT("/Game/Shared")));
    BlueprintAudit::SortFindings(SameEvidence);
    if (!TestEqual(TEXT("both same-evidence findings are present"), SameEvidence.Num(), 2))
    {
        return false;
    }
    TestEqual(TEXT("the message breaks an evidence tie"), SameEvidence[0].Message, FString(TEXT("Alpha message.")));

    return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
