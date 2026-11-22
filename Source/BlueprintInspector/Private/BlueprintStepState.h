#pragma once

#include "CoreMinimal.h"
#include "BlueprintInspectionTypes.h"
#include "BlueprintStepRules.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"
#include "UObject/WeakObjectPtr.h"

class UFunction;

// Where the next stop is allowed to be: the debugging mode, and the frame the current step started from.
//
// Written by DAP request threads (continue/step) and by the game thread at a stop, then read by the game
// thread on every dispatch, so every write and the snapshot go through one lock. A dispatch reads the
// mode first (lock-free; ContinueExecution is the common case) and takes the snapshot otherwise, so a
// concurrent step cannot half-apply.
class FBlueprintStepState
{
public:
    struct FSnapshot
    {
        EDebuggingMode Mode = EDebuggingMode::ContinueExecution;
        int32 StartDepth = 0;
        // Weak: a snapshot is a value that can outlive the class it names; resolve it at the comparison.
        TWeakObjectPtr<const UFunction> StartFunction;
        int64 StartAddress = INDEX_NONE;
    };

    // Any thread: the whole state as one locked read. The returned snapshot holds the armed function
    // weakly; a caller must resolve it rather than assuming the class is still alive.
    FSnapshot Observe() const;

    // Any thread: the lock-free mode. Every write to it happens under the lock.
    bool IsContinuing() const;

    // Any thread: a resume clears the step mode and keeps the armed frame, so a later step-out check
    // still has a depth.
    void Resume();

    // Any thread: records the mode a step request asked for. False when nothing is armed, when a
    // step-out has no caller to stop in (the paused frame is the root), or when the kind is not a
    // step this state handles.
    bool TryArmStep(EBlueprintStepKind Kind);

    // Game thread: a stop arms the frame the next step is measured from.
    void Arm(EDebuggingMode InMode, int32 InStartDepth, const UFunction* InStartFunction, int64 InStartAddress);

    // Teardown: back to "continue", with no frame armed.
    void Reset();

private:
    mutable FCriticalSection Lock;
    TAtomic<int32> Mode{static_cast<int32>(EDebuggingMode::ContinueExecution)};
    bool bArmed = false;
    int32 StartDepth = 0;
    TWeakObjectPtr<const UFunction> StartFunction;
    int64 StartAddress = INDEX_NONE;
};
