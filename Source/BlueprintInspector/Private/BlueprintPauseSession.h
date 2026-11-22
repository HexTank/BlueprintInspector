#pragma once

#include "CoreMinimal.h"
#include "BlueprintBreakpointTypes.h"
#include "BlueprintClassRegistry.h"
#include "BlueprintInspectionTypes.h"
#include "BlueprintPauseState.h"
#include "BlueprintPausedFrame.h"
#include "HAL/CriticalSection.h"
#include "HAL/Event.h"

class FProperty;
class UFunction;
class UObject;
struct FFrame;

// The paused VM view: the stopped call chain, the variable groups it exposes, and the generation that
// makes a stale reference fail instead of dereferencing freed stack memory.
//
// The pointers in FBlueprintPausedFrame are raw and not GC-visible; they are valid only while the game
// thread is parked and the frame is published. Publishing happens under one lock, so a DAP reader either
// sees a whole frame chain or none of it.
//
// Every read copies under the lock and never renders, so a request may call them while the game thread
// is parked. Publishing renders the classes it reports (through the registry) before it stores them,
// because the client asks for source as soon as the stopped event arrives.
class FBlueprintPauseSession
{
public:
    explicit FBlueprintPauseSession(FBlueprintClassRegistry& InRegistry);

    // One event per session. A request that copies the handle can safely trigger it even if the session
    // is reset at the same time, and the pool only gets the event back after the last reference is gone.
    // Named without the Win32 CreateEvent macro: windows.h maps that identifier to CreateEventW.
    void CreatePauseEvent();
    TSharedPtr<FEvent, ESPMode::ThreadSafe> GetEvent() const;
    TSharedPtr<FEvent, ESPMode::ThreadSafe> TakeEvent();

    // The lock-free hint. Every decision that touches a queue is made under RequestLock instead.
    bool IsPaused() const;
    bool IsPausedTakingRequestLock() const;
    void PublishPaused();
    // The pause ends: the flag clears and the queued writes are taken in one step, then canceled.
    void EndPause();

    // setBreakpoints while the VM is parked. The check and the enqueue are one step under the lock, so a
    // request either queues for a pause that will drain it, or is told to take the game-thread path.
    bool QueueBreakpointsIfPaused(FBlueprintPendingBreakpoints Pending);
    TArray<FBlueprintPendingBreakpoints> TakePendingBreakpoints();
    void DropPendingBreakpoints();

    // setVariable: applies now on the game thread, or queues for the parked loop and waits for the apply.
    FBlueprintSetVariableResult SetVariable(int32 VariablesReference, const FString& Name, const FString& Value);
    // Game thread, while parked: apply what DAP threads queued.
    void ProcessPendingWrites();

    // Game thread: publish the frame chain and the variable groups for the stopped frame.
    void PublishFrame(const FFrame& InFrame, int64 Address, int32 LineOverride);

#if WITH_DEV_AUTOMATION_TESTS
    TFunction<void()> TestBeforePublish;
#endif

    // The pause ends: drop the frame and its groups. A retained copy fails its generation check rather
    // than reading stack memory that has resumed.
    void ClearFrame();

    // Any thread: read the published frame.
    TArray<FBlueprintStackFrame> AcquireStackFrames() const;
    TArray<FBlueprintScope> AcquireScopes(int32 FrameId) const;
    TArray<FBlueprintVariable> AcquireVariables(int32 VariablesReference) const;
    FBlueprintEvaluateResult EvaluateExpression(int32 FrameId, const FString& Expression) const;

#if WITH_DEV_AUTOMATION_TESTS
    static int32 TakeNextReferenceForTest(int32& Counter, TFunctionRef<bool(int32)> IsInUse)
    {
        return TakeNextReference(Counter, IsInUse);
    }

    static int64 GetCallerFrameAddressForTest(const FFrame& InFrame);
#endif

    // Game thread: apply a variable write to a published group, and report what the value became.
    FBlueprintSetVariableResult SetVariableOnGameThread(int32 VariablesReference, const FString& Name, const FString& Value);

    // Any thread: the 1-based line for an address inside a paused function, falling back to the nearest
    // statement at or before it, and to 1 when the class has no render.
    int32 FindLineAtOrBefore(int32 ClassHandle, const UFunction* Function, int64 Address) const;

private:
    // Both expect the caller to hold Lock: the public reads take it once and use these inside.
    int32 AllocateGroupLocked(const FBlueprintVariableGroup& Group) const;
    FBlueprintVariable MakeVariableLocked(const FProperty* Property, const uint8* Container, const UObject* Parent, const FString& Name = FString(), EBlueprintVariableGroupAccess Access = EBlueprintVariableGroupAccess::Writable) const;

    // The exact line at the address, else the nearest statement at or before it, else 1.
    int32 FindLine(int32 ClassHandle, const UFunction* Function, int64 Address) const;

    static int32 TakeNextReference(int32& Counter, TFunctionRef<bool(int32)> IsInUse);

    FBlueprintClassRegistry& Registry;

    // Guards the pause flag, the write queue, the breakpoint queue and the event handle. Queuing work
    // and ending the pause are one transition under this lock, so a write or a breakpoint request can
    // never be left for a loop that has stopped draining.
    mutable FCriticalSection RequestLock;
    FBlueprintPauseState PauseState{RequestLock};
    TSharedPtr<FEvent, ESPMode::ThreadSafe> Event;
    TArray<FBlueprintPendingBreakpoints> PendingBreakpoints;

    mutable FCriticalSection Lock;
    uint64 Generation = 0;
    TArray<FBlueprintPausedFrame> Frames;
    // Mutable because a read of a nested variable allocates the child group it exposes; the lock still
    // serializes every access.
    mutable TMap<int32, FBlueprintVariableGroup> Groups;
    mutable int32 NextReference = 1;
};
