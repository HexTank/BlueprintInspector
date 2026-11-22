#include "BlueprintStepState.h"

#include "Misc/ScopeLock.h"
#include "UObject/Class.h"

FBlueprintStepState::FSnapshot FBlueprintStepState::Observe() const
{
    FScopeLock Guard(&Lock);

    FSnapshot Snapshot;
    Snapshot.Mode = static_cast<EDebuggingMode>(Mode.Load());
    Snapshot.StartDepth = StartDepth;
    Snapshot.StartFunction = StartFunction;
    Snapshot.StartAddress = StartAddress;
    return Snapshot;
}

bool FBlueprintStepState::IsContinuing() const
{
    return static_cast<EDebuggingMode>(Mode.Load()) == EDebuggingMode::ContinueExecution;
}

void FBlueprintStepState::Resume()
{
    FScopeLock Guard(&Lock);
    Mode.Store(static_cast<int32>(EDebuggingMode::ContinueExecution));
}

bool FBlueprintStepState::TryArmStep(const EBlueprintStepKind Kind)
{
    FScopeLock Guard(&Lock);

    if (!bArmed)
    {
        return false;
    }

    // There is no caller to stop in when the paused frame is the root.
    if (Kind == EBlueprintStepKind::Out && StartDepth == 0)
    {
        return false;
    }

    EDebuggingMode NewMode = EDebuggingMode::ContinueExecution;
    switch (Kind)
    {
    case EBlueprintStepKind::Over:
        NewMode = EDebuggingMode::StepOver;
        break;
    case EBlueprintStepKind::Into:
        NewMode = EDebuggingMode::StepInto;
        break;
    case EBlueprintStepKind::Out:
        NewMode = EDebuggingMode::StepOut;
        break;
    default:
        return false;
    }
    Mode.Store(static_cast<int32>(NewMode));
    return true;
}

void FBlueprintStepState::Arm(const EDebuggingMode InMode, const int32 InStartDepth, const UFunction* const InStartFunction, const int64 InStartAddress)
{
    FScopeLock Guard(&Lock);

    Mode.Store(static_cast<int32>(InMode));
    bArmed = true;
    StartDepth = InStartDepth;
    StartFunction = InStartFunction;
    StartAddress = InStartAddress;
}

void FBlueprintStepState::Reset()
{
    FScopeLock Guard(&Lock);

    Mode.Store(static_cast<int32>(EDebuggingMode::ContinueExecution));
    bArmed = false;
    StartDepth = 0;
    StartFunction.Reset();
    StartAddress = INDEX_NONE;
}
