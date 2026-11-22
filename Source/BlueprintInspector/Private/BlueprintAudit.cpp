// Copyright (c) 2024-2026 Paul Tankard

#include "BlueprintAudit.h"
#include "BlueprintInspector.h"
#include "BlueprintInspectorPort.h"
#include "BlueprintScriptDisassembler.h"
#include "BlueprintScriptVisualizer_Pseudo.h"

#include "Internationalization/Regex.h"
#include "Misc/App.h"
#include "UObject/Class.h"
#if BI_ENGINE_AT_LEAST_5_5
#include "UObject/StrProperty.h"
#endif
#include "UObject/Package.h"
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace
{
constexpr int32 MaxEvidenceLineLength = 200;
constexpr int32 MaxPackagesInMessage = 5;
constexpr int32 MaxAuditFindings = 5000;
constexpr int32 AssignmentSeparatorLength = 3; // " = "

struct FSecretPatterns
{
    FRegexPattern Assignment{TEXT("(?i)(password|passwd|pwd|api[_-]?key|apikey|secret|token|bearer|authorization|client_secret|private_key)\\s*[:=]")};
    FRegexPattern PrivateKey{TEXT("-----BEGIN [A-Z ]*PRIVATE KEY-----")};
    FRegexPattern AwsAccessKey{TEXT("\\bAKIA[0-9A-Z]{16}\\b")};
    FRegexPattern Jwt{TEXT("\\beyJ[A-Za-z0-9_-]{10,}\\.[A-Za-z0-9_-]{10,}\\.[A-Za-z0-9_-]{10,}\\b")};
    FRegexPattern UrlCredentials{TEXT("(?i)\\b(mysql|postgres(?:ql)?|mongodb(?:\\+srv)?|redis|amqp)://[^\\s\"]+:[^\\s\"]+@")};
    FRegexPattern HttpCredentials{TEXT("(?i)https?://[^\\s\"/@]+:[^\\s\"/@]+@")};
    FRegexPattern StripeKey{TEXT("\\bsk-[A-Za-z0-9]{20,}\\b")};
};

const FSecretPatterns& Secrets()
{
    static const FSecretPatterns Patterns;
    return Patterns;
}

bool Matches(const FRegexPattern& Pattern, const FString& Text)
{
    FRegexMatcher Matcher(Pattern, Text);
    return Matcher.FindNext();
}

bool PropertyNameLooksSecret(const FString& Name)
{
    // Boundaries keep compounds out: TokenLabel and TokenizeInput are not secrets, while auth_token and
    // Password are. A name-only hit is only a warning, so a missed spelling costs little.
    static const FRegexPattern Pattern(TEXT("(^|[^A-Za-z0-9])(?i:password|passwd|pwd|api[_-]?key|apikey|secret|token|credential|private[_-]?key)([^A-Za-z0-9]|$)"));
    return Matches(Pattern, Name);
}

struct FQuotedLiteral
{
    int32 RawStart = 0;
    int32 RawLength = 0;
    FString Value;
};

void ExtractQuotedLiterals(const FString& Line, TArray<FQuotedLiteral>& Out)
{
    for (int32 Index = 0; Index < Line.Len(); ++Index)
    {
        if (Line[Index] != TEXT('"'))
        {
            continue;
        }

        const int32 RawStart = Index + 1;
        FString Value;
        bool bClosed = false;
        for (++Index; Index < Line.Len(); ++Index)
        {
            const TCHAR Character = Line[Index];
            if (Character == TEXT('\\') && Index + 1 < Line.Len())
            {
                Value.AppendChar(Line[++Index]);
                continue;
            }
            if (Character == TEXT('"'))
            {
                bClosed = true;
                break;
            }
            Value.AppendChar(Character);
        }

        if (bClosed)
        {
            Out.Add({RawStart, Index - RawStart, MoveTemp(Value)});
        }
    }
}

void ExtractQuotedStrings(const FString& Line, TArray<FString>& Out)
{
    TArray<FQuotedLiteral> Literals;
    ExtractQuotedLiterals(Line, Literals);
    for (const FQuotedLiteral& Literal : Literals)
    {
        Out.Add(Literal.Value);
    }
}

FString TrimmedLine(const FString& Line)
{
    FString Trimmed = Line;
    Trimmed.TrimStartAndEndInline();
    return Trimmed.Left(MaxEvidenceLineLength);
}

// Evidence is shipped to the DAP client, so mask any quoted literal that looks like a credential even
// when the finding itself is about something else. Mask on the untrimmed line: TrimmedLine cuts at 200
// chars, so a literal that crosses the cut is not a substring of the trimmed copy and would ship as-is.
// Rebuild from the literals' own spans rather than replacing the raw text: a text replacement masks
// every identical occurrence and loses the tail of a longer literal that starts with a shorter secret.
FString RedactedEvidence(const FString& Line)
{
    TArray<FQuotedLiteral> Literals;
    ExtractQuotedLiterals(Line, Literals);

    FString Redacted;
    Redacted.Reserve(Line.Len());
    int32 Cursor = 0;
    for (const FQuotedLiteral& Literal : Literals)
    {
        Redacted.Append(Line.Mid(Cursor, Literal.RawStart - Cursor));
        if (Literal.RawLength > 0 && !BlueprintAudit::SecretPatternName(Literal.Value).IsEmpty())
        {
            Redacted.Append(BlueprintAudit::MaskSecret(Literal.Value));
        }
        else
        {
            Redacted.Append(Line.Mid(Literal.RawStart, Literal.RawLength));
        }
        Cursor = Literal.RawStart + Literal.RawLength;
    }
    Redacted.Append(Line.Mid(Cursor, Line.Len() - Cursor));
    FString Trimmed = TrimmedLine(Redacted);

    // A credential can sit outside a closed quoted literal (a bare URL or key) or in the tail after an
    // unterminated quote, neither of which the literal rewrite inspects. Re-check the rebuilt line before
    // TrimmedLine truncates it: a URL whose '@' falls past the cut would otherwise ship its user name and
    // part of its password.
    if (!BlueprintAudit::SecretPatternName(Redacted).IsEmpty())
    {
        return BlueprintAudit::MaskSecret(Redacted);
    }
    return Trimmed;
}

struct FScanContext
{
    FBlueprintAuditResult& Result;
    TSet<FString>& Seen;
    const int32 MaxFindings;

    // Parts of the input the scan could not read. They are reported as one BPI041 summary at the end of
    // the run, so a partial report cannot look complete.
    int32 SkippedFunctions = 0;
    int32 TruncatedDefaults = 0;
    FString FirstSkippedClass;
    FString FirstSkippedFunction;
    FString FirstTruncatedClass;
    FString FirstTruncatedPath;

    void Add(const FString& Rule, const EBlueprintAuditSeverity Severity, const FString& Category, const FString& ClassName, const FString& FunctionName, const int32 Line, const FString& Message, const FRedactedText& Evidence, const FString& Extra = FString())
    {
        // Dedupe before the budget check: a duplicate must not consume a finding slot or truncate the
        // scan. Severity and the redacted value are part of the key because class-default hits share
        // Line 0 and the property name, so an array's later elements would otherwise collapse into the
        // first one and hide the offending value.
        const FString Key = FString::Printf(TEXT("%s|%s|%s|%d|%s|%u|%s"), *Rule, *ClassName, *FunctionName, Line, *Extra, static_cast<uint8>(Severity), *Evidence.ToString());
        if (Seen.Contains(Key))
        {
            return;
        }

        if (Result.Findings.Num() >= MaxFindings)
        {
            Result.bTruncated = true;
            return;
        }
        Seen.Add(Key);

        FBlueprintAuditFinding& Finding = Result.Findings.AddDefaulted_GetRef();
        Finding.Rule = Rule;
        Finding.Severity = Severity;
        Finding.Category = Category;
        Finding.ClassName = ClassName;
        Finding.FunctionName = FunctionName;
        Finding.Line = Line;
        Finding.Message = Message;
        Finding.Evidence = Evidence;
    }
};

FString ClassNameOf(const UClass* const Class)
{
    return Class != nullptr ? Class->GetName() : FString();
}

bool MatchesDebugCall(const FString& Line)
{
    static const FRegexPattern Pattern(TEXT("\\b(PrintString|PrintText|DrawDebug[A-Za-z]*|Breakpoint)\\s*\\("));
    return Matches(Pattern, Line);
}

bool MatchesConsoleCall(const FString& Line)
{
    static const FRegexPattern Pattern(TEXT("\\b(ExecuteConsoleCommand|QuitGame|SetGamePaused|OpenLevel|LoadStreamLevel)\\s*\\("));
    return Matches(Pattern, Line);
}

bool MatchesExpensiveCall(const FString& Line)
{
    static const FRegexPattern Pattern(TEXT("\\b(GetAllActorsOfClass|GetAllActorsWithTag|GetAllActorsWithInterface|GetAllActorsOfClassWithTag|"
                                            "FindAllActors|LoadObject|AsyncLoadAsset|CreateWidget|SpawnEmitterAtLocation|PlaySoundAtLocation)\\s*\\("));
    return Matches(Pattern, Line);
}

bool IsIdentifierCharacter(const TCHAR Character)
{
    return FChar::IsAlnum(Character) || Character == TEXT('_');
}

bool IsIdentifier(const FString& Text)
{
    if (Text.IsEmpty() || !(FChar::IsAlpha(Text[0]) || Text[0] == TEXT('_')))
    {
        return false;
    }
    for (const TCHAR Character : Text)
    {
        if (!IsIdentifierCharacter(Character))
        {
            return false;
        }
    }
    return true;
}

// Whole-identifier match: K2Node_DynamicCast_1_bSuccess must not satisfy a search for
// K2Node_DynamicCast_bSuccess, and a name inside a longer identifier is a different variable.
bool ContainsIdentifier(const FString& Text, const FString& Identifier)
{
    for (int32 At = Text.Find(Identifier, ESearchCase::CaseSensitive); At != INDEX_NONE; At = Text.Find(Identifier, ESearchCase::CaseSensitive, ESearchDir::FromStart, At + 1))
    {
        const int32 End = At + Identifier.Len();
        const bool bStartsName = At == 0 || !IsIdentifierCharacter(Text[At - 1]);
        const bool bEndsName = End >= Text.Len() || !IsIdentifierCharacter(Text[End]);
        if (bStartsName && bEndsName)
        {
            return true;
        }
    }
    return false;
}

FString TrimmedLineCopy(const FString& Line)
{
    FString Trimmed = Line;
    Trimmed.TrimStartAndEndInline();
    return Trimmed;
}

// Splits "Name = Expression;" into its identifier and its right side. A comparison or a call is not an
// assignment, because its left side is not a bare identifier.
bool ParseAssignment(const FString& TrimmedLine, FString& OutLeft, FString& OutRight)
{
    const int32 EqualsAt = TrimmedLine.Find(TEXT(" = "));
    if (EqualsAt == INDEX_NONE)
    {
        return false;
    }
    const FString Left = TrimmedLine.Left(EqualsAt);
    if (!IsIdentifier(Left))
    {
        return false;
    }
    OutLeft = Left;
    OutRight = TrimmedLine.Mid(EqualsAt + AssignmentSeparatorLength);
    return true;
}

// A dynamic cast renders as "<temp> = Cast<Type>(operand);", followed by the node's own
// "<node>_bSuccess = <temp> != nullptr;". The temp is named after the node, which is the only per-cast
// identity the text carries: two casts in one function share the rule but not the temp. The prefix also
// keeps implicit numeric conversions out, because "K2Node_VariableSet_RealVar_ImplicitCast = Cast<double>(...)"
// is not a dynamic cast and cannot be null.
bool IsDynamicCastTempName(const FString& Name)
{
    return Name.StartsWith(TEXT("K2Node_DynamicCast_")) || Name.StartsWith(TEXT("K2Node_ClassDynamicCast_"));
}

// Only a condition tests a cast. The generated "<node>_bSuccess = <temp> != nullptr;" line copies the test
// into a variable; on its own it proves nothing, because the compiler emits it whether or not the graph
// branches on it.
bool IsConditionLine(const FString& TrimmedLine)
{
    return TrimmedLine.StartsWith(TEXT("if (")) || TrimmedLine.StartsWith(TEXT("else if (")) || TrimmedLine.StartsWith(TEXT("while (")) || TrimmedLine.StartsWith(TEXT("for ("));
}

FString BuildConfigurationName(const EBuildConfiguration Configuration)
{
    switch (Configuration)
    {
    case EBuildConfiguration::Debug:
        return TEXT("Debug");
    case EBuildConfiguration::DebugGame:
        return TEXT("DebugGame");
    case EBuildConfiguration::Development:
        return TEXT("Development");
    case EBuildConfiguration::Shipping:
        return TEXT("Shipping");
    case EBuildConfiguration::Test:
        return TEXT("Test");
    default:
        return TEXT("Unknown");
    }
}

// The default-value scan descends through every kind of property that can hold text: the string-like
// leaves, a soft reference's path, and the arrays, sets, maps and structs that nest them. Numeric,
// boolean, enum, object and delegate leaves hold no text, and an object reference is audited as its own
// class. That boundary is the audit's contract: text a future engine version puts somewhere else has to
// be added here, and the name a finding reports is the path to the exact slot it came from.
constexpr int32 MaxDefaultValueDepth = 8;

void ScanDefaultValue(FScanContext& Context, const UClass* const Class, const FProperty* const LeafProperty, const FString& Path, const FString& Value)
{
    if (Value.IsEmpty())
    {
        return;
    }

    const FString PatternName = BlueprintAudit::SecretPatternName(Value);
    // The leaf's own name drives the name heuristic: the path can contain a struct called Secrets whose
    // members are ordinary labels.
    const bool bNameLooksSecret = PropertyNameLooksSecret(LeafProperty->GetName());
    if (PatternName.IsEmpty() && !bNameLooksSecret)
    {
        return;
    }

    // Attribute an inherited default to the class that declares it: the same default scanned for each
    // derived class then produces one finding, not one per class in the chain.
    const UClass* const DeclaringClass = LeafProperty->GetOwnerClass();
    const FString ClassName = ClassNameOf(DeclaringClass != nullptr ? DeclaringClass : Class);

    // A value that matches a credential pattern is an error. A property whose name suggests a secret but
    // whose value looks ordinary is only a review prompt: without the split, TokenLabel, TokenizeInput
    // and PasswordPromptText are all reported as hardcoded secrets. The path is the dedupe key, so two
    // elements that carry the same value stay two findings.
    if (!PatternName.IsEmpty())
    {
        Context.Add(TEXT("BPI010"), EBlueprintAuditSeverity::Error, TEXT("Security"), ClassName, FString(), 0, FString::Printf(TEXT("Possible hardcoded secret in default value '%s' (%s)."), *Path, *PatternName), FRedactedText::Masked(Value), Path);
        return;
    }

    Context.Add(TEXT("BPI010"), EBlueprintAuditSeverity::Warning, TEXT("Security"), ClassName, FString(), 0, FString::Printf(TEXT("Default value '%s' has a secret-like name; verify it holds no credential."), *Path), FRedactedText::Masked(Value), Path);
}

void ScanPropertyValue(FScanContext& Context, const UClass* const Class, const FProperty* const Property, void* const Value, const FString& Path, const int32 Depth)
{
    if (Property == nullptr || Value == nullptr)
    {
        return;
    }
    if (Depth > MaxDefaultValueDepth)
    {
        ++Context.TruncatedDefaults;
        if (Context.FirstTruncatedPath.IsEmpty())
        {
            Context.FirstTruncatedClass = ClassNameOf(Class);
            Context.FirstTruncatedPath = Path;
        }
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Audit: default value nesting exceeded %d at '%s'; text below it is not scanned."), MaxDefaultValueDepth, *Path);
        return;
    }

    if (const FStrProperty* const StringProperty = CastField<FStrProperty>(Property))
    {
        ScanDefaultValue(Context, Class, Property, Path, StringProperty->GetPropertyValue(Value));
    }
    else if (const FNameProperty* const NameProperty = CastField<FNameProperty>(Property))
    {
        ScanDefaultValue(Context, Class, Property, Path, NameProperty->GetPropertyValue(Value).ToString());
    }
    else if (const FTextProperty* const TextProperty = CastField<FTextProperty>(Property))
    {
        ScanDefaultValue(Context, Class, Property, Path, TextProperty->GetPropertyValue(Value).ToString());
    }
    else if (const FSoftObjectProperty* const SoftObjectProperty = CastField<FSoftObjectProperty>(Property))
    {
        // A soft reference's value is a path string, so it can carry a URL with credentials.
        ScanDefaultValue(Context, Class, Property, Path, SoftObjectProperty->GetPropertyValue(Value).ToString());
    }
    else if (const FArrayProperty* const ArrayProperty = CastField<FArrayProperty>(Property))
    {
        FScriptArrayHelper Helper(ArrayProperty, Value);
        for (int32 Index = 0; Index < Helper.Num(); ++Index)
        {
            ScanPropertyValue(Context, Class, ArrayProperty->Inner, Helper.GetRawPtr(Index), FString::Printf(TEXT("%s[%d]"), *Path, Index), Depth + 1);
        }
    }
    else if (const FSetProperty* const SetProperty = CastField<FSetProperty>(Property))
    {
        FScriptSetHelper Helper(SetProperty, Value);
        for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
        {
            if (!Helper.IsValidIndex(Index))
            {
                continue;
            }
            ScanPropertyValue(Context, Class, SetProperty->ElementProp, Helper.GetElementPtr(Index), FString::Printf(TEXT("%s[%d]"), *Path, Index), Depth + 1);
        }
    }
    else if (const FMapProperty* const MapProperty = CastField<FMapProperty>(Property))
    {
        FScriptMapHelper Helper(MapProperty, Value);
        for (int32 Index = 0; Index < Helper.GetMaxIndex(); ++Index)
        {
            if (!Helper.IsValidIndex(Index))
            {
                continue;
            }
            const FString EntryPath = FString::Printf(TEXT("%s[%d]"), *Path, Index);
            ScanPropertyValue(Context, Class, MapProperty->KeyProp, Helper.GetKeyPtr(Index), FString::Printf(TEXT("%s.Key"), *EntryPath), Depth + 1);
            ScanPropertyValue(Context, Class, MapProperty->ValueProp, Helper.GetValuePtr(Index), FString::Printf(TEXT("%s.Value"), *EntryPath), Depth + 1);
        }
    }
    else if (const FStructProperty* const StructProperty = CastField<FStructProperty>(Property))
    {
        // Members are reached by offset, so a member that is itself a container or a struct is covered by
        // the same walk: ContainerStructVar.Lookup maps, StructVar.Str strings.
        for (TFieldIterator<FProperty> MemberIt(StructProperty->Struct); MemberIt; ++MemberIt)
        {
            const FProperty* const Member = *MemberIt;
            ScanPropertyValue(Context, Class, Member, Member->ContainerPtrToValuePtr<void>(Value), FString::Printf(TEXT("%s.%s"), *Path, *Member->GetName()), Depth + 1);
        }
    }
}

void ScanClassDefaults(FScanContext& Context, const UClass* const Class)
{
    UObject* const CDO = Class->GetDefaultObject();
    if (CDO == nullptr)
    {
        return;
    }

    for (TFieldIterator<FProperty> PropertyIt(Class); PropertyIt; ++PropertyIt)
    {
        const FProperty* const Property = *PropertyIt;
        ScanPropertyValue(Context, Class, Property, Property->ContainerPtrToValuePtr<void>(CDO), Property->GetName(), 0);
    }
}

bool ScanFunction(FScanContext& Context, const UClass* const Class, const UFunction* const Function, const bool bClassHasTick)
{
    FBlueprintScriptDisassembler Disassembler;
    if (!Disassembler.FunctionToNodes(TWeakObjectPtr<const UFunction>(Function)))
    {
        // Counted as scanned by the caller only when this succeeds: a function whose disassembly fails is
        // a hole in the audit, not a function that was audited. BPI041 reports the hole at the end.
        ++Context.SkippedFunctions;
        if (Context.FirstSkippedFunction.IsEmpty())
        {
            Context.FirstSkippedClass = ClassNameOf(Class);
            Context.FirstSkippedFunction = Function != nullptr ? Function->GetName() : FString();
        }
        return false;
    }

    FBlueprintScriptVisualizer_Pseudo Visualizer;
    FString Text;
    Visualizer.Visualize(TWeakObjectPtr<const UFunction>(Function), Disassembler, Text, nullptr);

    TArray<FString> Lines;
    Text.ParseIntoArrayLines(Lines, false);

    const FString ClassName = ClassNameOf(Class);
    const FString FunctionName = Function->GetName();
    const bool bTickFunction = FunctionName == TEXT("ReceiveTick");
    const bool bUbergraph = FunctionName.StartsWith(TEXT("ExecuteUbergraph"));
    const bool bReportExpensive = bTickFunction || (bClassHasTick && bUbergraph);

    // One finding per unchecked cast, not one per function: the totals the rule used before could hide
    // an unguarded cast beside a guarded one, and could not tell a guard for another cast apart.
    const TArray<BlueprintAudit::FUncheckedCast> UncheckedCasts = BlueprintAudit::FindUncheckedCasts(Lines);

    for (int32 Index = 0; Index < Lines.Num(); ++Index)
    {
        const FString& Line = Lines[Index];
        const int32 LineNumber = Index + 1;

        TArray<FString> Literals;
        ExtractQuotedStrings(Line, Literals);
        for (const FString& Literal : Literals)
        {
            const FString PatternName = BlueprintAudit::SecretPatternName(Literal);
            if (!PatternName.IsEmpty())
            {
                // The masked evidence is "<redacted: N char(s)>", so two same-length secrets would collide in
                // the dedupe key. The raw literal only feeds a local set; hash it into the key.
                Context.Add(TEXT("BPI010"), EBlueprintAuditSeverity::Error, TEXT("Security"), ClassName, FunctionName, LineNumber, FString::Printf(TEXT("Possible hardcoded secret in a string literal (%s)."), *PatternName), FRedactedText::Masked(Literal), FString::Printf(TEXT("%s|%u"), *PatternName, GetTypeHash(Literal)));
            }
        }

        if (MatchesDebugCall(Line))
        {
            Context.Add(TEXT("BPI020"), EBlueprintAuditSeverity::Warning, TEXT("Shipping"), ClassName, FunctionName, LineNumber, TEXT("Debug-only call left in a Blueprint."), FRedactedText::RedactedLine(Line));
        }

        if (MatchesConsoleCall(Line))
        {
            Context.Add(TEXT("BPI021"), EBlueprintAuditSeverity::Error, TEXT("Security"), ClassName, FunctionName, LineNumber, TEXT("Console or level-control call; review whether a player can reach it."), FRedactedText::RedactedLine(Line));
        }

        if (bReportExpensive && MatchesExpensiveCall(Line))
        {
            Context.Add(TEXT("BPI030"), bTickFunction ? EBlueprintAuditSeverity::Warning : EBlueprintAuditSeverity::Info, TEXT("Performance"), ClassName, FunctionName, LineNumber, bTickFunction ? TEXT("Expensive call in Event Tick.") : TEXT("Expensive call in the event ubergraph of a class that implements Tick; "
                                                                                                                                                                                                                                                 "verify it is not on the Tick path."),
                        FRedactedText::RedactedLine(Line));
        }
    }

    for (const BlueprintAudit::FUncheckedCast& Cast : UncheckedCasts)
    {
        Context.Add(TEXT("BPI031"), EBlueprintAuditSeverity::Info, TEXT("Correctness"), ClassName, FunctionName, Cast.Line, FString::Printf(TEXT("The cast result '%s' has no success check; verify it is null-guarded."), *Cast.Variable), FRedactedText::RedactedLine(Lines[Cast.Line - 1]), Cast.Variable);
    }

    return true;
}

void ScanDuplicateNames(FScanContext& Context, const TArray<UClass*>& Classes)
{
    TMap<FName, TArray<UClass*>> ByName;
    for (UClass* const Class : Classes)
    {
        ByName.FindOrAdd(Class->GetFName()).AddUnique(Class);
    }

    for (const TPair<FName, TArray<UClass*>>& Pair : ByName)
    {
        if (Pair.Value.Num() < 2)
        {
            continue;
        }

        TArray<FString> Packages;
        for (const UClass* const Class : Pair.Value)
        {
            Packages.AddUnique(Class->GetOutermost()->GetName());
        }
        if (Packages.Num() < 2)
        {
            continue;
        }

        // Sorted and de-duplicated: the class list and the grouping map arrive in engine order, which is
        // not a stable order between runs, and the message is part of the report.
        const FString PackageList = BlueprintAudit::FormatPackageList(Packages, MaxPackagesInMessage);

        for (UClass* const Class : Pair.Value)
        {
            // The dedupe key in FScanContext::Add is Rule|ClassName|FunctionName|Line|Extra, and Extra
            // defaults to empty for the other rules. The package path keeps one class per package in
            // the key; repeating the short name (shared by the whole group) would drop all but the first.
            const FString PackageName = Class->GetOutermost()->GetName();
            Context.Add(TEXT("BPI040"), EBlueprintAuditSeverity::Warning, TEXT("Hygiene"), Class->GetName(), FString(), 0, FString::Printf(TEXT("Class short name is used by %d packages (%s); "
                                                                                                                                                "name-based lookups can pick the wrong class."),
                                                                                                                                           Packages.Num(), *PackageList),
                        FRedactedText::Authored(PackageName), PackageName);
        }
    }
}
} // namespace

FRedactedText FRedactedText::Authored(const FString& Text)
{
#if !UE_BUILD_SHIPPING
    // Authored text is the escape hatch; a secret here would ship verbatim. Catch the misuse in a
    // development build instead of silently trusting the call site.
    ensureMsgf(!BlueprintAudit::ContainsSecretPattern(Text), TEXT("FRedactedText::Authored received text that matches a secret pattern; use Masked()"));
#endif
    return FRedactedText(Text);
}

FRedactedText FRedactedText::Masked(const FString& Value)
{
    return FRedactedText(BlueprintAudit::MaskSecret(Value));
}

FRedactedText FRedactedText::RedactedLine(const FString& Line)
{
    return FRedactedText(RedactedEvidence(Line));
}

namespace BlueprintAudit
{
FString SecretPatternName(const FString& Value)
{
    const FSecretPatterns& Patterns = Secrets();
    if (Matches(Patterns.PrivateKey, Value))
    {
        return TEXT("private key");
    }
    if (Matches(Patterns.AwsAccessKey, Value))
    {
        return TEXT("AWS access key");
    }
    if (Matches(Patterns.Jwt, Value))
    {
        return TEXT("JWT");
    }
    if (Matches(Patterns.UrlCredentials, Value) || Matches(Patterns.HttpCredentials, Value))
    {
        return TEXT("URL with credentials");
    }
    if (Matches(Patterns.StripeKey, Value))
    {
        return TEXT("API key");
    }
    if (Matches(Patterns.Assignment, Value))
    {
        return TEXT("key/value assignment");
    }
    return FString();
}

FString MaskSecret(const FString& Value)
{
    return FString::Printf(TEXT("<redacted: %d char(s)>"), Value.Len());
}

const TCHAR* SeverityToString(const EBlueprintAuditSeverity Severity)
{
    switch (Severity)
    {
    case EBlueprintAuditSeverity::Error:
        return TEXT("error");
    case EBlueprintAuditSeverity::Warning:
        return TEXT("warning");
    default:
        return TEXT("info");
    }
}

void SortFindings(TArray<FBlueprintAuditFinding>& Findings)
{
    Findings.Sort([](const FBlueprintAuditFinding& A, const FBlueprintAuditFinding& B) {
        if (A.Severity != B.Severity)
        {
            return static_cast<uint8>(A.Severity) > static_cast<uint8>(B.Severity);
        }
        if (A.Category != B.Category)
        {
            return A.Category < B.Category;
        }
        if (A.Rule != B.Rule)
        {
            return A.Rule < B.Rule;
        }
        if (A.ClassName != B.ClassName)
        {
            return A.ClassName < B.ClassName;
        }
        if (A.FunctionName != B.FunctionName)
        {
            return A.FunctionName < B.FunctionName;
        }
        if (A.Line != B.Line)
        {
            return A.Line < B.Line;
        }
        // Evidence and message last, so the order is total. Every duplicate-name finding of one group
        // shares class, function and line, and only the package in its evidence tells them apart:
        // without this the group's entries swap positions between runs.
        if (A.Evidence != B.Evidence)
        {
            return A.Evidence < B.Evidence;
        }
        return A.Message < B.Message;
    });
}

FString FormatPackageList(const TArray<FString>& Packages, const int32 MaxShown)
{
    TArray<FString> Sorted;
    Sorted.Reserve(Packages.Num());
    for (const FString& Package : Packages)
    {
        Sorted.AddUnique(Package);
    }
    Sorted.Sort();

    FString List;
    if (MaxShown <= 0)
    {
        return List;
    }
    for (int32 Index = 0; Index < Sorted.Num(); ++Index)
    {
        if (Index == MaxShown)
        {
            List += TEXT(", ...");
            break;
        }
        List += (Index == 0 ? TEXT("") : TEXT(", ")) + Sorted[Index];
    }
    return List;
}

namespace
{
struct FGuardExpr
{
    enum class EKind : uint8
    {
        Variable,
        Not,
        And,
        Or,
        Literal
    };

    EKind Kind = EKind::Literal;
    bool bLiteral = false;
    FString Name;
    TSharedPtr<FGuardExpr> Left;
    TSharedPtr<FGuardExpr> Right;
};

using FGuardExprPtr = TSharedPtr<FGuardExpr>;
using FGuardAlias = TPair<FString, bool>;

FGuardExprPtr MakeGuardVariable(const FString& Name)
{
    FGuardExprPtr Expr = MakeShared<FGuardExpr>();
    Expr->Kind = FGuardExpr::EKind::Variable;
    Expr->Name = Name;
    return Expr;
}

FGuardExprPtr MakeGuardNot(const FGuardExprPtr& Inner)
{
    if (!Inner.IsValid())
    {
        return nullptr;
    }
    FGuardExprPtr Expr = MakeShared<FGuardExpr>();
    Expr->Kind = FGuardExpr::EKind::Not;
    Expr->Left = Inner;
    return Expr;
}

FGuardExprPtr MakeGuardLiteral(const bool bValue)
{
    FGuardExprPtr Expr = MakeShared<FGuardExpr>();
    Expr->Kind = FGuardExpr::EKind::Literal;
    Expr->bLiteral = bValue;
    return Expr;
}

FGuardExprPtr MakeGuardBinary(const FGuardExpr::EKind Kind, const FGuardExprPtr& Left, const FGuardExprPtr& Right)
{
    if (!Left.IsValid() || !Right.IsValid())
    {
        return nullptr;
    }
    FGuardExprPtr Expr = MakeShared<FGuardExpr>();
    Expr->Kind = Kind;
    Expr->Left = Left;
    Expr->Right = Right;
    return Expr;
}

struct FGuardToken
{
    enum class EType : uint8
    {
        Identifier,
        Not,
        And,
        Or,
        LParen,
        RParen,
        Equal,
        NotEqual,
        Invalid,
        End
    };

    EType Type = EType::End;
    FString Text;
};

bool IsGuardNameCharacter(const TCHAR Character)
{
    return FChar::IsAlnum(Character) || Character == TEXT('_') || Character == TEXT('.') || Character == TEXT('[') || Character == TEXT(']');
}

void TokenizeGuard(const FString& Text, TArray<FGuardToken>& Out)
{
    int32 Index = 0;
    while (Index < Text.Len())
    {
        const TCHAR Character = Text[Index];
        if (FChar::IsWhitespace(Character))
        {
            ++Index;
            continue;
        }
        const auto AddToken = [&Out](const FGuardToken::EType Type, const FString& Value) {
            FGuardToken& Token = Out.AddDefaulted_GetRef();
            Token.Type = Type;
            Token.Text = Value;
        };
        if (Character == TEXT('&') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('&'))
        {
            AddToken(FGuardToken::EType::And, TEXT("&&"));
            Index += 2;
            continue;
        }
        if (Character == TEXT('|') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('|'))
        {
            AddToken(FGuardToken::EType::Or, TEXT("||"));
            Index += 2;
            continue;
        }
        if (Character == TEXT('=') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('='))
        {
            AddToken(FGuardToken::EType::Equal, TEXT("=="));
            Index += 2;
            continue;
        }
        if (Character == TEXT('!') && Index + 1 < Text.Len() && Text[Index + 1] == TEXT('='))
        {
            AddToken(FGuardToken::EType::NotEqual, TEXT("!="));
            Index += 2;
            continue;
        }
        if (Character == TEXT('!'))
        {
            AddToken(FGuardToken::EType::Not, TEXT("!"));
            ++Index;
            continue;
        }
        if (Character == TEXT('('))
        {
            AddToken(FGuardToken::EType::LParen, TEXT("("));
            ++Index;
            continue;
        }
        if (Character == TEXT(')'))
        {
            AddToken(FGuardToken::EType::RParen, TEXT(")"));
            ++Index;
            continue;
        }
        if (IsGuardNameCharacter(Character))
        {
            const int32 Start = Index;
            while (Index < Text.Len() && IsGuardNameCharacter(Text[Index]))
            {
                ++Index;
            }
            AddToken(FGuardToken::EType::Identifier, Text.Mid(Start, Index - Start));
            continue;
        }
        AddToken(FGuardToken::EType::Invalid, FString(1, &Character));
        ++Index;
    }
    FGuardToken& End = Out.AddDefaulted_GetRef();
    End.Type = FGuardToken::EType::End;
}

struct FGuardParser
{
    const TArray<FGuardToken>& Tokens;
    int32 Index = 0;

    explicit FGuardParser(const TArray<FGuardToken>& InTokens) : Tokens(InTokens)
    {
    }

    const FGuardToken& Peek() const
    {
        return Tokens[Index];
    }

    bool Parse(FGuardExprPtr& Out)
    {
        Out = ParseOr();
        return Out.IsValid() && Peek().Type == FGuardToken::EType::End;
    }

    FGuardExprPtr ParseOr()
    {
        FGuardExprPtr Left = ParseAnd();
        while (Left.IsValid() && Peek().Type == FGuardToken::EType::Or)
        {
            ++Index;
            Left = MakeGuardBinary(FGuardExpr::EKind::Or, Left, ParseAnd());
        }
        return Left;
    }

    FGuardExprPtr ParseAnd()
    {
        FGuardExprPtr Left = ParseUnary();
        while (Left.IsValid() && Peek().Type == FGuardToken::EType::And)
        {
            ++Index;
            Left = MakeGuardBinary(FGuardExpr::EKind::And, Left, ParseUnary());
        }
        return Left;
    }

    FGuardExprPtr ParseUnary()
    {
        if (Peek().Type == FGuardToken::EType::Not)
        {
            ++Index;
            return MakeGuardNot(ParseUnary());
        }
        return ParsePrimary();
    }

    FGuardExprPtr ParsePrimary()
    {
        const FGuardToken Token = Peek();
        if (Token.Type == FGuardToken::EType::LParen)
        {
            ++Index;
            FGuardExprPtr Inner = ParseOr();
            if (!Inner.IsValid() || Peek().Type != FGuardToken::EType::RParen)
            {
                return nullptr;
            }
            ++Index;
            return Inner;
        }
        if (Token.Type != FGuardToken::EType::Identifier)
        {
            return nullptr;
        }
        ++Index;
        const FString Name = Token.Text;
        if (Peek().Type == FGuardToken::EType::LParen)
        {
            ++Index;
            FGuardExprPtr Inner = ParseOr();
            if (!Inner.IsValid() || Peek().Type != FGuardToken::EType::RParen)
            {
                return nullptr;
            }
            ++Index;
            if (Name.Equals(TEXT("IsValid"), ESearchCase::IgnoreCase))
            {
                return Inner;
            }
            if (Name.Equals(TEXT("Not_PreBool"), ESearchCase::IgnoreCase))
            {
                return MakeGuardNot(Inner);
            }
            return nullptr;
        }
        if (Peek().Type == FGuardToken::EType::Equal || Peek().Type == FGuardToken::EType::NotEqual)
        {
            const bool bEqual = Peek().Type == FGuardToken::EType::Equal;
            ++Index;
            if (Peek().Type != FGuardToken::EType::Identifier)
            {
                return nullptr;
            }
            const FString Value = Peek().Text;
            ++Index;
            const bool bValueIsFalse = Value.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("null"), ESearchCase::IgnoreCase) || Value.Equals(TEXT("nullptr"), ESearchCase::IgnoreCase);
            const bool bValueIsTrue = Value.Equals(TEXT("true"), ESearchCase::IgnoreCase);
            if (!bValueIsFalse && !bValueIsTrue)
            {
                return nullptr;
            }
            const bool bVariableIsTrue = bEqual ? bValueIsTrue : !bValueIsTrue;
            const FGuardExprPtr Variable = MakeGuardVariable(Name);
            return bVariableIsTrue ? Variable : MakeGuardNot(Variable);
        }
        if (Name.Equals(TEXT("true"), ESearchCase::IgnoreCase))
        {
            return MakeGuardLiteral(true);
        }
        if (Name.Equals(TEXT("false"), ESearchCase::IgnoreCase))
        {
            return MakeGuardLiteral(false);
        }
        return MakeGuardVariable(Name);
    }
};

bool IsGuardDerivationRight(const FString& Right)
{
    return Right.Contains(TEXT("nullptr")) || Right.Contains(TEXT("IsValid(")) || Right.Contains(TEXT("Not_PreBool("));
}

bool IsNegativeGuardDerivation(const FString& Right)
{
    return Right.Contains(TEXT("Not_PreBool(")) || Right.Contains(TEXT("== nullptr")) || Right.Contains(TEXT("!= true"));
}

void BuildGuardAliases(const TArray<FString>& Lines, TMap<FString, FGuardAlias>& Out)
{
    for (const FString& Line : Lines)
    {
        const FString Trimmed = TrimmedLineCopy(Line);
        FString Left;
        FString Right;
        if (!ParseAssignment(Trimmed, Left, Right))
        {
            continue;
        }
        if (IsDynamicCastTempName(Left) && Right.StartsWith(TEXT("Cast<")))
        {
            Out.Add(Left, FGuardAlias(Left, true));
            continue;
        }
        if (!IsGuardDerivationRight(Right))
        {
            continue;
        }
        FString Base;
        bool bBasePolarity = true;
        FString BestKey;
        bool bFound = false;
        for (const TPair<FString, FGuardAlias>& Pair : Out)
        {
            if (!ContainsIdentifier(Right, Pair.Key))
            {
                continue;
            }
            const bool bBetter = !bFound || Pair.Key.Len() > BestKey.Len() || (Pair.Key.Len() == BestKey.Len() && Pair.Key < BestKey);
            if (bBetter)
            {
                bFound = true;
                BestKey = Pair.Key;
                Base = Pair.Value.Key;
                bBasePolarity = Pair.Value.Value;
            }
        }
        if (!bFound)
        {
            continue;
        }
        const bool bNegative = IsNegativeGuardDerivation(Right);
        Out.Add(Left, FGuardAlias(Base, bBasePolarity != bNegative));
    }
}

FGuardExprPtr ResolveGuardAliases(const FGuardExprPtr& Expr, const TMap<FString, FGuardAlias>& Aliases)
{
    if (!Expr.IsValid())
    {
        return nullptr;
    }
    switch (Expr->Kind)
    {
    case FGuardExpr::EKind::Variable:
    {
        const FGuardAlias* const Alias = Aliases.Find(Expr->Name);
        if (Alias == nullptr)
        {
            return Expr;
        }
        const FGuardExprPtr Base = MakeGuardVariable(Alias->Key);
        return Alias->Value ? Base : MakeGuardNot(Base);
    }
    case FGuardExpr::EKind::Not:
        return MakeGuardNot(ResolveGuardAliases(Expr->Left, Aliases));
    case FGuardExpr::EKind::And:
    case FGuardExpr::EKind::Or:
        return MakeGuardBinary(Expr->Kind, ResolveGuardAliases(Expr->Left, Aliases), ResolveGuardAliases(Expr->Right, Aliases));
    case FGuardExpr::EKind::Literal:
        return Expr;
    }
    return nullptr;
}

bool EvaluateGuard(const FGuardExprPtr& Expr, const TMap<FString, bool>& Assignment)
{
    if (!Expr.IsValid())
    {
        return false;
    }
    switch (Expr->Kind)
    {
    case FGuardExpr::EKind::Variable:
    {
        const bool* const Found = Assignment.Find(Expr->Name);
        return Found != nullptr && *Found;
    }
    case FGuardExpr::EKind::Not:
        return !EvaluateGuard(Expr->Left, Assignment);
    case FGuardExpr::EKind::And:
        return EvaluateGuard(Expr->Left, Assignment) && EvaluateGuard(Expr->Right, Assignment);
    case FGuardExpr::EKind::Or:
        return EvaluateGuard(Expr->Left, Assignment) || EvaluateGuard(Expr->Right, Assignment);
    case FGuardExpr::EKind::Literal:
        return Expr->bLiteral;
    }
    return false;
}

void CollectGuardVariables(const FGuardExprPtr& Expr, TSet<FString>& Out)
{
    if (!Expr.IsValid())
    {
        return;
    }
    switch (Expr->Kind)
    {
    case FGuardExpr::EKind::Variable:
        Out.Add(Expr->Name);
        break;
    case FGuardExpr::EKind::Not:
        CollectGuardVariables(Expr->Left, Out);
        break;
    case FGuardExpr::EKind::And:
    case FGuardExpr::EKind::Or:
        CollectGuardVariables(Expr->Left, Out);
        CollectGuardVariables(Expr->Right, Out);
        break;
    case FGuardExpr::EKind::Literal:
        break;
    }
}

struct FGuardConstraint
{
    FGuardExprPtr Expr;
    bool bExpected = true;
};

struct FGuardNode
{
    bool bStatement = false;
    int32 Line = INDEX_NONE;
    FGuardExprPtr ParsedCondition;
    bool bLoop = false;
    TArray<TSharedPtr<FGuardNode>> Body;
    TArray<TSharedPtr<FGuardNode>> ElseBody;
    bool bHasElse = false;
    int32 BodyFirst = INDEX_NONE;
    int32 BodyLast = INDEX_NONE;
    int32 ElseFirst = INDEX_NONE;
    int32 ElseLast = INDEX_NONE;
    int32 LastLine = INDEX_NONE;
    bool bBodyExits = false;
    bool bElseExits = false;
};

bool StartsWithKeyword(const FString& Text, const TCHAR* const Keyword)
{
    const int32 KeywordLength = FCString::Strlen(Keyword);
    if (!Text.StartsWith(Keyword))
    {
        return false;
    }
    return Text.Len() == KeywordLength || FChar::IsWhitespace(Text[KeywordLength]) || Text[KeywordLength] == TEXT(';') || Text[KeywordLength] == TEXT('(');
}

bool IsExitStatement(const FString& Trimmed)
{
    return StartsWithKeyword(Trimmed, TEXT("return")) || StartsWithKeyword(Trimmed, TEXT("continue")) || StartsWithKeyword(Trimmed, TEXT("break"));
}

bool NodesExit(const TArray<FString>& Lines, const TArray<TSharedPtr<FGuardNode>>& Nodes)
{
    if (Nodes.Num() == 0)
    {
        return false;
    }
    const TSharedPtr<FGuardNode>& Last = Nodes.Last();
    if (!Last.IsValid())
    {
        return false;
    }
    if (Last->bStatement)
    {
        return IsExitStatement(TrimmedLineCopy(Lines[Last->Line]));
    }
    if (Last->bLoop)
    {
        return false;
    }
    return Last->bHasElse && Last->bBodyExits && Last->bElseExits;
}

int32 FindMatchingBrace(const TArray<FString>& Lines, const int32 OpenLine, const int32 End)
{
    int32 Depth = 0;
    for (int32 Line = OpenLine; Line < End; ++Line)
    {
        for (const TCHAR Character : Lines[Line])
        {
            if (Character == TEXT('{'))
            {
                ++Depth;
            }
            else if (Character == TEXT('}'))
            {
                --Depth;
                if (Depth == 0)
                {
                    return Line;
                }
            }
        }
    }
    return INDEX_NONE;
}

int32 NextNonEmptyLine(const TArray<FString>& Lines, int32 Line, const int32 End)
{
    while (Line < End && TrimmedLineCopy(Lines[Line]).IsEmpty())
    {
        ++Line;
    }
    return Line < End ? Line : INDEX_NONE;
}

FString ExtractConditionText(const FString& Trimmed)
{
    const int32 Open = Trimmed.Find(TEXT("("));
    const int32 Close = Trimmed.Find(TEXT(")"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
    if (Open == INDEX_NONE || Close == INDEX_NONE || Close <= Open)
    {
        return FString();
    }
    return Trimmed.Mid(Open + 1, Close - Open - 1);
}

void ParseGuardBlock(const TArray<FString>& Lines, int32& Index, int32 End, const TMap<FString, FGuardAlias>& Aliases, TArray<TSharedPtr<FGuardNode>>& Out);
void ParseGuardIf(const TArray<FString>& Lines, int32& Index, int32 End, const TMap<FString, FGuardAlias>& Aliases, TArray<TSharedPtr<FGuardNode>>& Out);

void ParseGuardIf(const TArray<FString>& Lines, int32& Index, const int32 End, const TMap<FString, FGuardAlias>& Aliases, TArray<TSharedPtr<FGuardNode>>& Out)
{
    const FString Trimmed = TrimmedLineCopy(Lines[Index]);
    const bool bLoop = Trimmed.StartsWith(TEXT("while (")) || Trimmed.StartsWith(TEXT("for ("));
    const int32 IfLine = Index;
    const FString ConditionText = ExtractConditionText(Trimmed);
    FGuardExprPtr Parsed;
    if (!ConditionText.IsEmpty())
    {
        TArray<FGuardToken> Tokens;
        TokenizeGuard(ConditionText, Tokens);
        FGuardParser Parser(Tokens);
        FGuardExprPtr Raw;
        if (Parser.Parse(Raw))
        {
            Parsed = ResolveGuardAliases(Raw, Aliases);
        }
    }
    ++Index;
    const int32 OpenLine = NextNonEmptyLine(Lines, Index, End);
    if (OpenLine == INDEX_NONE || TrimmedLineCopy(Lines[OpenLine]) != TEXT("{"))
    {
        TSharedPtr<FGuardNode> Statement = MakeShared<FGuardNode>();
        Statement->bStatement = true;
        Statement->Line = IfLine;
        Out.Add(Statement);
        return;
    }
    const int32 CloseLine = FindMatchingBrace(Lines, OpenLine, End);
    if (CloseLine == INDEX_NONE)
    {
        Index = End;
        return;
    }
    TSharedPtr<FGuardNode> Node = MakeShared<FGuardNode>();
    Node->Line = IfLine;
    Node->ParsedCondition = Parsed;
    Node->bLoop = bLoop;
    Node->BodyFirst = OpenLine + 1;
    Node->BodyLast = CloseLine - 1;
    Node->LastLine = CloseLine;
    int32 BodyIndex = OpenLine + 1;
    ParseGuardBlock(Lines, BodyIndex, CloseLine, Aliases, Node->Body);
    Node->bBodyExits = NodesExit(Lines, Node->Body);
    Out.Add(Node);
    Index = CloseLine + 1;

    const int32 ElseLine = NextNonEmptyLine(Lines, Index, End);
    if (ElseLine == INDEX_NONE)
    {
        return;
    }
    const FString ElseTrimmed = TrimmedLineCopy(Lines[ElseLine]);
    if (ElseTrimmed.StartsWith(TEXT("else if (")))
    {
        TArray<TSharedPtr<FGuardNode>> Nested;
        int32 NestedIndex = ElseLine;
        ParseGuardIf(Lines, NestedIndex, End, Aliases, Nested);
        Node->bHasElse = true;
        Node->ElseFirst = ElseLine;
        Node->ElseLast = NestedIndex - 1;
        Node->ElseBody = MoveTemp(Nested);
        Node->bElseExits = NodesExit(Lines, Node->ElseBody);
        Node->LastLine = Node->ElseLast;
        Index = NestedIndex;
    }
    else if (ElseTrimmed == TEXT("else"))
    {
        const int32 ElseOpen = NextNonEmptyLine(Lines, ElseLine + 1, End);
        if (ElseOpen == INDEX_NONE || TrimmedLineCopy(Lines[ElseOpen]) != TEXT("{"))
        {
            return;
        }
        const int32 ElseClose = FindMatchingBrace(Lines, ElseOpen, End);
        if (ElseClose == INDEX_NONE)
        {
            Index = End;
            return;
        }
        Node->bHasElse = true;
        Node->ElseFirst = ElseOpen + 1;
        Node->ElseLast = ElseClose - 1;
        Node->LastLine = ElseClose;
        int32 ElseIndex = ElseOpen + 1;
        ParseGuardBlock(Lines, ElseIndex, ElseClose, Aliases, Node->ElseBody);
        Node->bElseExits = NodesExit(Lines, Node->ElseBody);
        Index = ElseClose + 1;
    }
}

void ParseGuardBlock(const TArray<FString>& Lines, int32& Index, const int32 End, const TMap<FString, FGuardAlias>& Aliases, TArray<TSharedPtr<FGuardNode>>& Out)
{
    while (Index < End)
    {
        const FString Trimmed = TrimmedLineCopy(Lines[Index]);
        if (Trimmed.IsEmpty() || Trimmed == TEXT("{") || Trimmed == TEXT("}"))
        {
            ++Index;
            continue;
        }
        if (Trimmed.StartsWith(TEXT("if (")) || Trimmed.StartsWith(TEXT("while (")) || Trimmed.StartsWith(TEXT("for (")))
        {
            ParseGuardIf(Lines, Index, End, Aliases, Out);
            continue;
        }
        TSharedPtr<FGuardNode> Statement = MakeShared<FGuardNode>();
        Statement->bStatement = true;
        Statement->Line = Index;
        Out.Add(Statement);
        ++Index;
    }
}

bool FindGuardPath(const TArray<TSharedPtr<FGuardNode>>& Nodes, const int32 Line, TArray<FGuardConstraint>& Path)
{
    for (const TSharedPtr<FGuardNode>& NodePtr : Nodes)
    {
        if (!NodePtr.IsValid())
        {
            continue;
        }
        const FGuardNode& Node = *NodePtr;
        if (Node.bStatement)
        {
            if (Node.Line == Line)
            {
                return true;
            }
            continue;
        }
        if (Line >= Node.BodyFirst && Line <= Node.BodyLast)
        {
            if (Node.ParsedCondition.IsValid())
            {
                Path.Add(FGuardConstraint{Node.ParsedCondition, true});
            }
            return FindGuardPath(Node.Body, Line, Path);
        }
        if (Node.bHasElse && Line >= Node.ElseFirst && Line <= Node.ElseLast)
        {
            if (Node.ParsedCondition.IsValid())
            {
                Path.Add(FGuardConstraint{Node.ParsedCondition, false});
            }
            return FindGuardPath(Node.ElseBody, Line, Path);
        }
        if (Line > Node.LastLine)
        {
            if (Node.bLoop || !Node.ParsedCondition.IsValid())
            {
                continue;
            }
            if (!Node.bHasElse)
            {
                if (Node.bBodyExits)
                {
                    Path.Add(FGuardConstraint{Node.ParsedCondition, false});
                }
            }
            else if (Node.bBodyExits && !Node.bElseExits)
            {
                Path.Add(FGuardConstraint{Node.ParsedCondition, false});
            }
            else if (!Node.bBodyExits && Node.bElseExits)
            {
                Path.Add(FGuardConstraint{Node.ParsedCondition, true});
            }
        }
    }
    return false;
}

bool AccessIsGuarded(const TArray<FGuardConstraint>& Path, const FString& Temp)
{
    if (Path.Num() == 0)
    {
        return false;
    }
    TSet<FString> Variables;
    for (const FGuardConstraint& Constraint : Path)
    {
        CollectGuardVariables(Constraint.Expr, Variables);
    }
    Variables.Add(Temp);
    if (Variables.Num() > 10)
    {
        return false;
    }
    TArray<FString> Names;
    Names.Reserve(Variables.Num());
    for (const FString& Name : Variables)
    {
        Names.Add(Name);
    }
    const int32 Count = 1 << Names.Num();
    bool bReachable = false;
    for (int32 Mask = 0; Mask < Count; ++Mask)
    {
        TMap<FString, bool> Assignment;
        for (int32 Index = 0; Index < Names.Num(); ++Index)
        {
            Assignment.Add(Names[Index], (Mask & (1 << Index)) != 0);
        }
        bool bMatches = true;
        for (const FGuardConstraint& Constraint : Path)
        {
            if (EvaluateGuard(Constraint.Expr, Assignment) != Constraint.bExpected)
            {
                bMatches = false;
                break;
            }
        }
        if (!bMatches)
        {
            continue;
        }
        bReachable = true;
        const bool* const TempValue = Assignment.Find(Temp);
        if (TempValue == nullptr || !*TempValue)
        {
            return false;
        }
    }
    return bReachable;
}
} // namespace

TArray<FUncheckedCast> FindUncheckedCasts(const TArray<FString>& Lines)
{
    TArray<FString> TrimmedLines;
    TrimmedLines.Reserve(Lines.Num());
    for (const FString& Line : Lines)
    {
        TrimmedLines.Add(TrimmedLineCopy(Line));
    }

    TMap<FString, FGuardAlias> Aliases;
    BuildGuardAliases(Lines, Aliases);

    TArray<TSharedPtr<FGuardNode>> Root;
    int32 RootIndex = 0;
    ParseGuardBlock(Lines, RootIndex, Lines.Num(), Aliases, Root);

    struct FTrackedCast
    {
        int32 Line = 0;
        FString Temp;
        bool bUnguarded = false;
    };

    TArray<FTrackedCast> Casts;
    for (int32 LineIndex = 0; LineIndex < TrimmedLines.Num(); ++LineIndex)
    {
        const FString& Trimmed = TrimmedLines[LineIndex];
        FString Left;
        FString Right;
        if (ParseAssignment(Trimmed, Left, Right) && IsDynamicCastTempName(Left) && Right.StartsWith(TEXT("Cast<")))
        {
            FTrackedCast& Cast = Casts.AddDefaulted_GetRef();
            Cast.Line = LineIndex + 1;
            Cast.Temp = Left;
        }
    }

    for (FTrackedCast& Cast : Casts)
    {
        for (int32 LineIndex = 0; LineIndex < TrimmedLines.Num(); ++LineIndex)
        {
            if (LineIndex == Cast.Line - 1)
            {
                continue;
            }
            const FString& Trimmed = TrimmedLines[LineIndex];
            if (!ContainsIdentifier(Trimmed, Cast.Temp) || IsConditionLine(Trimmed))
            {
                continue;
            }
            FString Left;
            FString Right;
            if (ParseAssignment(Trimmed, Left, Right) && IsGuardDerivationRight(Right))
            {
                continue;
            }
            TArray<FGuardConstraint> Path;
            if (!FindGuardPath(Root, LineIndex, Path) || !AccessIsGuarded(Path, Cast.Temp))
            {
                Cast.bUnguarded = true;
                break;
            }
        }
    }

    TArray<FUncheckedCast> Unchecked;
    for (const FTrackedCast& Cast : Casts)
    {
        if (Cast.bUnguarded)
        {
            FUncheckedCast& Entry = Unchecked.AddDefaulted_GetRef();
            Entry.Line = Cast.Line;
            Entry.Variable = Cast.Temp;
        }
    }
    return Unchecked;
}

FBlueprintAuditResult Run(const TArray<UObject*>& Classes, const int32 MaxFindings)
{
    check(IsInGameThread());

    FBlueprintAuditResult Result;
    TSet<FString> Seen;
    FScanContext Context{Result, Seen, FMath::Clamp(MaxFindings, 1, MaxAuditFindings)};

    const EBuildConfiguration Configuration = FApp::GetBuildConfiguration();
    const bool bShipping = Configuration == EBuildConfiguration::Shipping;
    // The audit runs inside the plugin module, so a "module loaded" test is always true. Report the build
    // and the plugin's presence; the listener's state is a session concern, not a module concern.
    const FString Bpi001Message = FString::Printf(TEXT("BlueprintInspector is compiled into this %s build. The DAP listener starts with the game subsystem; exclude the plugin from Shipping packages unless you intend to ship it."), *BuildConfigurationName(Configuration));
    Context.Add(TEXT("BPI001"), bShipping ? EBlueprintAuditSeverity::Error : EBlueprintAuditSeverity::Warning, TEXT("Shipping"), FString(), FString(), 0, Bpi001Message, FRedactedText::Authored(TEXT("See SECURITY.md for the listener and token model.")));

    TArray<UClass*> ClassPointers;
    for (UObject* const Object : Classes)
    {
        if (UClass* const Class = Cast<UClass>(Object))
        {
            ClassPointers.AddUnique(Class);
        }
    }

    ScanDuplicateNames(Context, ClassPointers);

    for (UClass* const Class : ClassPointers)
    {
        if (Result.bTruncated)
        {
            break;
        }

        ++Result.ClassesScanned;
        ScanClassDefaults(Context, Class);

        const UFunction* const TickFunction = Class->FindFunctionByName(TEXT("ReceiveTick"));
        const bool bClassHasTick = TickFunction != nullptr && TickFunction->Script.Num() > 0;

        for (TFieldIterator<UFunction> FunctionIt(Class, EFieldIteratorFlags::ExcludeSuper); FunctionIt; ++FunctionIt)
        {
            const UFunction* const Function = *FunctionIt;
            if (Function->Script.Num() == 0)
            {
                continue;
            }
            if (ScanFunction(Context, Class, Function, bClassHasTick))
            {
                ++Result.FunctionsScanned;
            }

            if (Result.bTruncated)
            {
                break;
            }
        }
    }

    if (Context.SkippedFunctions > 0 || Context.TruncatedDefaults > 0)
    {
        FString FirstGap;
        if (!Context.FirstSkippedFunction.IsEmpty())
        {
            FirstGap = FString::Printf(TEXT("%s.%s (disassembly)"), *Context.FirstSkippedClass, *Context.FirstSkippedFunction);
        }
        else
        {
            FirstGap = FString::Printf(TEXT("%s.%s (default nesting)"), *Context.FirstTruncatedClass, *Context.FirstTruncatedPath);
        }
        const FString Message = FString::Printf(
            TEXT("The audit is partial: %d function(s) could not be disassembled and %d default value(s) were nested deeper than %d. First gap: %s."),
            Context.SkippedFunctions, Context.TruncatedDefaults, MaxDefaultValueDepth, *FirstGap);
        const FString ClassName = !Context.FirstSkippedClass.IsEmpty() ? Context.FirstSkippedClass : Context.FirstTruncatedClass;
        Context.Add(TEXT("BPI041"), EBlueprintAuditSeverity::Warning, TEXT("Hygiene"), ClassName, Context.FirstSkippedFunction, 0, Message,
                    FRedactedText::Authored(TEXT("The audit skipped part of the input; fix the disassembly or nesting and re-run.")));
    }

    SortFindings(Result.Findings);

    return Result;
}
} // namespace BlueprintAudit
