#pragma once

#include "CoreMinimal.h"

struct FFunctionProperties
{
    FName Name;
    int32 ID = 0;
};

// What a breakpoint request says about the source it names: the class name from `source.path` is the
// identity; the handle is a cache key that may be replayed from an earlier session.
struct FBlueprintSourceIdentity
{
    FString ClassName;
    int32 SourceReference = 0;
};

struct FBlueprintBreakpointResolution
{
    int32 RequestedLine = 0;
    // The line that was armed, or 0 when nothing at or below the requested line carries an address.
    int32 Line = 0;
    // False when the request was resolved with no source map at all (the class has not been rendered or
    // its cache is empty), as opposed to a rendered source with no breakpointable line at that request.
    bool bSourceAvailable = false;
};

// One requested breakpoint with the DAP options VS Code sends for it. A logpoint is a breakpoint with
// a LogMessage and must not stop.
struct FBlueprintBreakpointRequest
{
    int32 Line = 0;
    FString Condition;
    FString HitCondition;
    FString LogMessage;
};

struct FBlueprintStackFrame
{
    FString Name;
    FString ClassName;
    int32 SourceReference = 0;
    int32 Line = 1;
};

struct FBlueprintScope
{
    FString Name;
    int32 VariablesReference = 0;
};

struct FBlueprintVariable
{
    FString Name;
    FString Value;
    FString Type;
    int32 VariablesReference = 0;
};

struct FBlueprintSetVariableResult
{
    bool bSuccess = false;
    bool bOutcomeUnknown = false;
    FString Value;
    FString Type;
    int32 VariablesReference = 0;
    FString Error;
};

// Hot-path capture. Counts are per rendered line; a capture is owned by the DAP connection, so the
// adapter clears it on disconnect and the extension only enables it through an authenticated request.
struct FBlueprintHotCaptureStatus
{
    bool bCapturing = false;
    uint32 Generation = 0;
    int64 TotalHits = 0;
};

struct FBlueprintCaptureMetadata
{
    FString Map;
    FString Build;
    FString Engine;
    FString Project;
    FString CapturedAt;
};

struct FBlueprintHotLine
{
    FString ClassName;
    FString FunctionName;
    int32 Line = 0;
    int64 Hits = 0;
    bool bTransfer = false;
};

struct FBlueprintHotLinesResult
{
    FBlueprintHotCaptureStatus Status;
    TArray<FBlueprintHotLine> Lines;
    // True when the response cap dropped lines the capture actually recorded.
    bool bTruncated = false;
};

struct FBlueprintEvaluateResult
{
    bool bSuccess = false;
    FString Result;
    FString Type;
    int32 VariablesReference = 0;
    FString Error;
};

enum class EBlueprintStepKind : uint8
{
    Over,
    Into,
    Out,
};

struct FBlueprintCoverageLine
{
    FString ClassName;
    FString ClassPath;
    FString FunctionName;
    int32 Line = 0;
    int64 Hits = 0;
    bool bTransfer = false;
};

struct FBlueprintCoverageResult
{
    FBlueprintHotCaptureStatus Status;
    TArray<FBlueprintCoverageLine> Lines;
    int32 RenderedClasses = 0;
    int32 TotalLines = 0;
    int32 CoveredLines = 0;
    int32 UncoveredLines = 0;
    bool bTruncated = false;
};
