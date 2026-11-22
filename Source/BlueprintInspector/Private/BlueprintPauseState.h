#pragma once

#include "CoreMinimal.h"

#include "BlueprintInspectionTypes.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include <chrono>

#include <condition_variable>
#include <exception>
#include <mutex>

// A setVariable request that arrived on a DAP thread while the game thread was parked. The object owns
// the queue/claim/cancel handshake: the game thread claims it before applying, and the waiter cancels
// only while it is still queued. The claim state and the completion are single-shot, so exactly one
// path runs the work and exactly one path reports the result.
class FBlueprintVariableWriteRequest
{
public:
    FBlueprintVariableWriteRequest(const int32 InVariablesReference, FString InName, FString InValue) : VariablesReference(InVariablesReference), Name(MoveTemp(InName)), Value(MoveTemp(InValue))
    {
    }

    // Waiter side. On timeout, cancel only if the game thread has not claimed it; a claimed write gets
    // a short grace period so a late completion is reported instead of a failure the VM contradicts.
    FBlueprintSetVariableResult Wait(const FTimespan Timeout)
    {
        if (WaitForCompletion(std::chrono::duration<double>(Timeout.GetTotalSeconds())))
        {
            return ReadResult();
        }

        // Timed out. Whichever of this cancel and the game thread's claim wins decides completion; the
        // loser must not complete a second time.
        int32 Expected = static_cast<int32>(EState::Queued);
        if (State.CompareExchange(Expected, static_cast<int32>(EState::Canceled)))
        {
            const FBlueprintSetVariableResult TimedOut = MakeError(TEXT("The game thread did not apply the variable write before the timeout."));
            Complete(TimedOut);
            return TimedOut;
        }

        if (WaitForCompletion(std::chrono::seconds(1)))
        {
            return ReadResult();
        }
        FBlueprintSetVariableResult Unknown;
        Unknown.bOutcomeUnknown = true;
        Unknown.Error = TEXT("The variable write was claimed but did not report within the grace period; refresh the variables and retry.");
        return Unknown;
    }

    // Game thread. Runs Work only if the request is still queued; when the claim is lost, the path that
    // took the state (a cancel or an earlier apply) owns completion.
    void Apply(const TFunctionRef<FBlueprintSetVariableResult()>& Work)
    {
        int32 Expected = static_cast<int32>(EState::Queued);
        if (!State.CompareExchange(Expected, static_cast<int32>(EState::Claimed)))
        {
            return;
        }

        FBlueprintSetVariableResult Outcome;
        try
        {
            Outcome = Work();
        }
        catch (const std::exception& Exception)
        {
            Outcome = MakeError(FString::Printf(TEXT("The variable write threw: %s"), UTF8_TO_TCHAR(Exception.what())));
        }
        catch (...)
        {
            Outcome = MakeError(TEXT("The variable write threw an unknown exception."));
        }
        Complete(Outcome);
    }

    // A request whose waiter timed out or was canceled before the game thread claimed it.
    bool IsCanceled() const
    {
        return State.Load() == static_cast<int32>(EState::Canceled);
    }

    // Resume path. A claimed request is left to the game thread, which owns completion.
    void Cancel()
    {
        int32 Expected = static_cast<int32>(EState::Queued);
        if (State.CompareExchange(Expected, static_cast<int32>(EState::Canceled)))
        {
            Complete(MakeError(TEXT("Execution resumed before the variable write was applied.")));
        }
    }

    int32 GetVariablesReference() const { return VariablesReference; }
    const FString& GetName() const { return Name; }
    const FString& GetValue() const { return Value; }

#if WITH_DEV_AUTOMATION_TESTS
    // The stress test asserts one completion per request; nothing else reads this.
    int32 GetCompletionCount() const
    {
        std::lock_guard<std::mutex> Lock(DoneMutex);
        return CompletionCount;
    }
#endif

private:
    enum class EState : int32
    {
        Queued = 0,
        Claimed = 1,
        Canceled = 2,
    };

    static FBlueprintSetVariableResult MakeError(FString Error)
    {
        FBlueprintSetVariableResult Result;
        Result.Error = MoveTemp(Error);
        return Result;
    }

    // Single-shot: the first caller wins and every later caller is a no-op, so the result a waiter sees
    // cannot be overwritten by a path that lost the claim race.
    void Complete(const FBlueprintSetVariableResult& InResult)
    {
        {
            std::lock_guard<std::mutex> Lock(DoneMutex);
            if (bCompleted)
            {
                return;
            }
            bCompleted = true;
            Result = InResult;
#if WITH_DEV_AUTOMATION_TESTS
            ++CompletionCount;
#endif
        }
        DoneCv.notify_all();
    }

    template <typename DurationType>
    bool WaitForCompletion(const DurationType Timeout)
    {
        std::unique_lock<std::mutex> Lock(DoneMutex);
        return DoneCv.wait_for(Lock, Timeout, [this]() { return bCompleted; });
    }

    FBlueprintSetVariableResult ReadResult()
    {
        std::lock_guard<std::mutex> Lock(DoneMutex);
        return Result;
    }

    int32 VariablesReference = 0;
    FString Name;
    FString Value;
    TAtomic<int32> State{static_cast<int32>(EState::Queued)};

    // Per-request (not from the thread-local event pool, which must be returned on the creating thread).
    mutable std::mutex DoneMutex;
    std::condition_variable DoneCv;
    bool bCompleted = false;
    FBlueprintSetVariableResult Result;
#if WITH_DEV_AUTOMATION_TESTS
    int32 CompletionCount = 0;
#endif
};

// The pause handshake: the paused flag and the setVariable queue are one state under one lock, which the
// subsystem passes in (its BreakpointRequestLock). The DAP side only enqueues while the flag is set, and
// the resume clears the flag and takes the queue in one step, so a write is never queued for a parked
// loop that has already stopped draining. Observe() is the lock-free hint; every decision that touches
// the queue is made under the lock.
class FBlueprintPauseState
{
public:
    using FWriteRequest = TSharedPtr<FBlueprintVariableWriteRequest>;

    // Bound for the parked-VM write queue; a full queue is reported to the caller as a rejection.
    static constexpr int32 MaxQueuedWrites = 256;

    enum class EWriteEnqueueResult : uint8
    {
        Queued,
        NotPaused,
        QueueFull
    };

    explicit FBlueprintPauseState(FCriticalSection& InLock) : Lock(InLock)
    {
    }

    bool Observe() const { return bPaused.Load(); }

    bool ObserveLocked() const
    {
        FScopeLock Scope(&Lock);
        return bPaused.Load();
    }

    // DAP thread. The answer and the enqueue are one step: false when the VM is not parked, so the caller
    // reports "not paused" instead of waiting for an apply that nothing will run.
    EWriteEnqueueResult EnqueueIfPaused(const FWriteRequest& Request)
    {
        FScopeLock Scope(&Lock);
        if (!bPaused.Load())
        {
            return EWriteEnqueueResult::NotPaused;
        }

        // Drop requests whose waiter already gave up, then bound the queue: a client cannot grow it
        // without limit while the VM stays parked.
        QueuedWrites.RemoveAll([](const FWriteRequest& Queued) {
            return !Queued.IsValid() || Queued->IsCanceled();
        });
        if (QueuedWrites.Num() >= MaxQueuedWrites)
        {
            return EWriteEnqueueResult::QueueFull;
        }

        QueuedWrites.Add(Request);
        return EWriteEnqueueResult::Queued;
    }

    // Game thread, while parked. Changing the flag is also a queue transition.
    void PublishPaused()
    {
        FScopeLock Scope(&Lock);
        bPaused.Store(true);
    }

    // Game thread, parked: take what is queued and leave the pause in place.
    TArray<FWriteRequest> TakeQueuedWrites()
    {
        FScopeLock Scope(&Lock);
        TArray<FWriteRequest> Taken = MoveTemp(QueuedWrites);
        QueuedWrites.Reset();
        return Taken;
    }

    // The pause ends. Clearing the flag and taking the queue are one transition: a write either queued
    // before it, and the caller cancels it, or it sees the flag clear and is rejected. A request already
    // claimed by the game thread is in flight and survives this transition.
    TArray<FWriteRequest> EndPause()
    {
        FScopeLock Scope(&Lock);
        bPaused.Store(false);
        TArray<FWriteRequest> Taken = MoveTemp(QueuedWrites);
        QueuedWrites.Reset();
        return Taken;
    }

private:
    FCriticalSection& Lock;
    TAtomic<bool> bPaused{false};
    TArray<FWriteRequest> QueuedWrites;
};
