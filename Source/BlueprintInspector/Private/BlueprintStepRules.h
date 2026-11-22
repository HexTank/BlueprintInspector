#pragma once

#include "CoreMinimal.h"

// How the debugger resumes after a stop.
enum class EDebuggingMode : uint8
{
    ContinueExecution,
    StepOver,
    StepInto,
    StepOut
};

inline bool ShouldStopForStep(const EDebuggingMode Mode, const int32 CurrentDepth, const int32 StartDepth, const bool bStatementAddress)
{
    if (!bStatementAddress)
    {
        return false;
    }

    switch (Mode)
    {
    case EDebuggingMode::StepInto:
        return true;
    case EDebuggingMode::StepOver:
        return CurrentDepth <= StartDepth;
    case EDebuggingMode::StepOut:
        return CurrentDepth < StartDepth;
    case EDebuggingMode::ContinueExecution:
        return false;
    }
    return false;
}

inline bool IsBreakpointAtStepStart(const EDebuggingMode Mode, const bool bBreakpointHit, const bool bSameFunction, const bool bSameAddress)
{
    switch (Mode)
    {
    case EDebuggingMode::ContinueExecution:
        return false;
    case EDebuggingMode::StepOver:
    case EDebuggingMode::StepInto:
    case EDebuggingMode::StepOut:
        return bBreakpointHit && bSameFunction && bSameAddress;
    }
    return false;
}
