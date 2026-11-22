// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"

// Order matters: findings are sorted by descending underlying value (Error first).
enum class EBlueprintAuditSeverity : uint8
{
    Info = 0,
    Warning = 1,
    Error = 2
};

class FRedactedText
{
public:
    FRedactedText() = default;

    // Code-authored, non-sensitive text only (a package path, a fixed guidance string). In a
    // development build a secret pattern reports a misuse through ensureMsgf and the text is returned
    // unmasked; use Masked() for values.
    static FRedactedText Authored(const FString& Text);
    static FRedactedText Masked(const FString& Value);
    static FRedactedText RedactedLine(const FString& Line);

    const FString& ToString() const { return Text; }
    bool operator==(const FRedactedText& Other) const { return Text == Other.Text; }
    bool operator!=(const FRedactedText& Other) const { return Text != Other.Text; }
    bool operator<(const FRedactedText& Other) const { return Text < Other.Text; }

private:
    explicit FRedactedText(const FString& InText) : Text(InText) {}

    FString Text;
};

struct FBlueprintAuditFinding
{
    FString Rule;
    EBlueprintAuditSeverity Severity = EBlueprintAuditSeverity::Info;
    FString Category;
    FString ClassName;
    FString FunctionName;
    int32 Line = 0;
    FString Message;
    FRedactedText Evidence;
};

struct FBlueprintAuditResult
{
    TArray<FBlueprintAuditFinding> Findings;
    int32 ClassesScanned = 0;
    int32 FunctionsScanned = 0;
    bool bTruncated = false;
};

/**
 * Heuristic pre-ship checks over Blueprint bytecode and class defaults. Findings are review prompts,
 * not proof of a vulnerability; the rules and how to add one are documented in CONTRIBUTING.md.
 *
 * The class-default scan reads every string-like value a default can hold: strings, names and text,
 * a soft reference's path, and the same at any depth through arrays, sets, maps and structs. It does
 * not read object references (the referenced class is audited on its own) or numeric, boolean, enum
 * and delegate leaves, none of which can hold text. A finding names the path to the slot it came from,
 * e.g. MapVar[0].Value or StructVar.Str.
 *
 * Run must run on the game thread: it walks UObject graphs, disassembles UFunction scripts and reads
 * class default objects. The pattern matchers beside it are pure text work.
 */
namespace BlueprintAudit
{
/** The rule name for the pattern that matched, or an empty string. */
FString SecretPatternName(const FString& Value);

/** True when Value looks like a credential rather than a plain identifier or URL. */
inline bool ContainsSecretPattern(const FString& Value)
{
    return !SecretPatternName(Value).IsEmpty();
}

/** A redacted form safe to show in a report; never includes the value itself. */
FString MaskSecret(const FString& Value);

const TCHAR* SeverityToString(EBlueprintAuditSeverity Severity);

/**
 * Sorts findings into the order Run reports them. The order is total: two findings that share severity,
 * category, rule, class, function and line still compare on evidence and message, so the report does not
 * vary between runs. Duplicate-name findings share all of the first six keys and differ only in evidence.
 */
void SortFindings(TArray<FBlueprintAuditFinding>& Findings);

/**
 * Formats the package list of a duplicate-name finding: sorted, de-duplicated, then capped at MaxShown
 * entries with a trailing ", ...". Sorting is what makes the message stable, because the caller collects
 * the packages in whatever order the class list and its set produce.
 */
FString FormatPackageList(const TArray<FString>& Packages, int32 MaxShown);

/** One dynamic cast with a result read on a path no condition proves succeeded. */
struct FUncheckedCast
{
    /** 1-based rendered line of the cast statement. */
    int32 Line = 0;
    /** The compiler temp the cast wrote, which is what tells two casts in one function apart. */
    FString Variable;
};

/**
 * Finds the dynamic casts whose result is read on a path where no condition implies success. A cast
 * whose result is never read is not reported: there is nothing to null-guard. Each read is identified
 * by the temp the cast wrote, and it is guarded only when the path to it includes an if/while condition
 * that reads that cast's own result, its generated success flag, or a flag derived from either (a
 * null-test copy, IsValid, a Not_PreBool of the flag). The generated "success = temp != nullptr" line is
 * a copy of the test, not a branch, so it registers a flag without counting as the check. Exposed so the
 * rule can be pinned without an authored fixture graph.
 */
TArray<FUncheckedCast> FindUncheckedCasts(const TArray<FString>& Lines);

/**
 * Audits the Blueprint classes in Classes and returns at most MaxFindings findings (clamped to
 * [1, 5000]). Once the budget is exhausted bTruncated is set and the remaining classes and functions
 * are skipped, so ClassesScanned/FunctionsScanned describe the audited subset, not the whole input.
 */
FBlueprintAuditResult Run(const TArray<UObject*>& Classes, int32 MaxFindings);
} // namespace BlueprintAudit
