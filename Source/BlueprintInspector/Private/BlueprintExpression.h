// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"

class UObject;
class UFunction;

/**
 * The debugger's expression subset.
 *
 * Supported:
 *   - property paths: `This.Health`, `IntVar`, `This.Stats.Score`, `This.Items[2].Name`
 *   - a single comparison: `This.Health <= 0`, `Name == "Bob"`, `Other == This.Target`
 *   - truthiness: `This.bEnabled`, `Target`, a literal (`true`, `0`, `"text"`, `null`), or any of
 *     those prefixed with `!`
 *
 * A leading `!` applies to the whole comparison, so `!This.bFlag == false` means
 * `!(This.bFlag == false)`; only one leading `!` is supported.
 *
 * Not supported (yet): `&&`, `||`, arithmetic and function calls. Logpoints substitute `{expr}`
 * placeholders with the value's display text.
 *
 * The entry points that take an FBlueprintExpressionContext read live UObject and FFrame memory, so
 * they are valid only while the pause that published that frame is still in place, and a returned value
 * inherits that window: ObjectValue is a raw pointer to an object the pause kept alive. The DAP evaluate
 * request calls them from its own thread for exactly that reason; FBlueprintPauseSession::EvaluateExpression
 * checks the frame under PausedLock first. EvaluateHitCondition takes no context and stays pure text work.
 */
struct FBlueprintExpressionContext
{
    const UObject* Object = nullptr;
    const UFunction* Function = nullptr;
    // FFrame::Locals for the stopped frame; parameters and locals are addressed relative to it.
    const uint8* Locals = nullptr;
};

struct FBlueprintExpressionValue
{
    bool bValid = false;
    FString Display;
    FString Type;

    bool bBoolValue = false;
    bool bIsBool = false;
    double Number = 0.0;
    bool bIsNumeric = false;
    FString Text;
    bool bIsText = false;
    const UObject* ObjectValue = nullptr;
    bool bIsObject = false;
};

namespace BlueprintExpression
{
/** Resolves a value path, a `!` negation, a comparison, or a `!=` test. A condition or a log message
 * placeholder uses the same resolver. Unlike EvaluateCondition, an empty expression is an error here.
 * OutValue is reset on entry, so only inspect it when this returns true. */
bool EvaluateValue(const FBlueprintExpressionContext& Context, const FString& Expression, FBlueprintExpressionValue& OutValue, FString& OutError);

/** Evaluates a condition (truthiness, `!`, or one comparison). An empty expression is true and clears
 * OutError. */
bool EvaluateCondition(const FBlueprintExpressionContext& Context, const FString& Expression, bool& bOutResult, FString& OutError);

/**
 * Replaces `{path}` placeholders with their display text; errors are shown inline. `{{` and `}}` emit
 * a single literal brace, so a message can contain braces without opening a placeholder.
 */
FString FormatLogMessage(const FBlueprintExpressionContext& Context, const FString& Message);

/** Parses a DAP hitCondition (`5`, `>=5`, `<=5`, `>5`, `<5`, `==5`, `%5`) against a 1-based count. An
 * empty condition breaks on every hit; `%0` and non-integer text are rejected and return false. */
bool EvaluateHitCondition(const FString& Condition, int64 HitCount, bool& bOutShouldBreak);
} // namespace BlueprintExpression
