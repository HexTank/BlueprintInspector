#pragma once

#include "CoreMinimal.h"

#if PLATFORM_WINDOWS
#include "Delegates/Delegate.h"
#include "Engine/EngineBaseTypes.h"
#include "HAL/CriticalSection.h"
#include "Templates/Atomic.h"

static_assert(sizeof(void*) == 8, "The UWorld hook ABI requires 64-bit Windows.");

struct FFrame;
class UObject;
class UFunction;
class UWorld;

class FBlueprintInspectorDetours
{
public:
    // True while the UWorld hooks are installed. A second Initialize is a no-op. Deinitialize always runs
    // the whole teardown: if the detach itself fails, the UWorld hooks stay installed and this keeps
    // returning true (so a later Initialize does not attach them twice) while the dispatch table, the
    // delegates and the debugging gate are still torn down.
    static bool IsInitialized();

    // Both entry points must run on the game thread. Teardown does not require the VM to be idle: the
    // rewriter enlists every thread in the process, so none of them can be inside a half-rewritten
    // prologue, and a dispatch that copied a handler just before the teardown drops it (see Notify*).
    // Deinitialize detaches the UWorld hooks, restores the dispatch table, clears the delegates and
    // disables debugging, so a later Initialize starts from the same state as the first one.
    static void Initialize();
    static void Deinitialize();

    // The engine may replace table entries after the wrap (hot reload or a late native registration).
    // Wrap them again before arming breakpoints. Game thread only, while IsInitialized(), and only when the
    // VM is not executing: idle, or parked at a stop, which is where ApplyBreakpoints calls it. A no-op
    // otherwise.
    static void Refresh();

    // Game thread: installs the dispatcher for an active debug session. Initialize attaches the
    // trampolines; Activate wraps the dispatch table and Deactivate restores the native one.
    static void Activate();
    static void Deactivate();

    // Gates the detoured UWorld::Tick/IsPaused passthrough and the stop logic. Written on the game
    // thread by the subsystem around a stop and read from the thread that ticks the world; accessors
    // keep it from being flipped independently of the hook lifecycle. Arming is refused while the hooks
    // are not installed, because no opcode would reach the wrapper anyway.
    static bool IsEnabled();
    static void SetEnabled(bool bInEnabled);

    // The VM reports every Blueprint opcode dispatch to these streams, after FFrame::Step consumes the
    // opcode byte and before its handler runs.
    //
    // The hooks execute on whatever thread runs the bytecode, normally the game thread while paused.
    // Registration and removal are serialised by an internal lock; the hook copies the delegates and
    // invokes them outside it, so a parked stop cannot block teardown. The streams are process-global:
    // the step/jump handlers are single-owner (a bind by a different owner is rejected), while the
    // observation streams accept any number of observers.
    DECLARE_DELEGATE_TwoParams(FOnScriptStep, const UObject*, const FFrame&);
    DECLARE_MULTICAST_DELEGATE_TwoParams(FScriptStepObserved, const UFunction*, int64);
    DECLARE_DELEGATE_ThreeParams(FOnScriptJump, const UObject*, const FFrame&, int64);
    DECLARE_MULTICAST_DELEGATE_ThreeParams(FScriptJumpObserved, const UFunction*, int64 /*JumpAddress*/, int64 /*Landing*/);

    // Single-owner handlers. The first parameter of both is the frame's context object ('this'), which
    // may be null; JumpAddress is the opcode offset the step handler saw before the dispatch. Bind and
    // unbind under the hook lock so the VM never executes a half-updated delegate.
    template <typename UserClass>
    static bool BindScriptStep(UserClass* InObject, void (UserClass::*InMethod)(const UObject*, const FFrame&))
    {
        FScopeLock Lock(&HookLock);
        if (InObject == nullptr || InMethod == nullptr)
        {
            return false;
        }
        if (StepHandler.IsBound() && StepHandler.GetUObject() != InObject)
        {
            return false;
        }
        StepHandler.BindUObject(InObject, InMethod);
        RefreshConsumerCount();
        return true;
    }

    template <typename UserClass>
    static bool BindScriptJump(UserClass* InObject, void (UserClass::*InMethod)(const UObject*, const FFrame&, int64))
    {
        FScopeLock Lock(&HookLock);
        if (InObject == nullptr || InMethod == nullptr)
        {
            return false;
        }
        if (JumpHandler.IsBound() && JumpHandler.GetUObject() != InObject)
        {
            return false;
        }
        JumpHandler.BindUObject(InObject, InMethod);
        RefreshConsumerCount();
        return true;
    }

    static void UnbindScriptStep();
    static void UnbindScriptJump();

    // Observation-only streams: they never pause or consult breakpoints. Add/remove under the same lock
    // the hook uses; pass the returned handle to the matching Remove.
    static FDelegateHandle AddScriptStepObserver(FScriptStepObserved::FDelegate InDelegate);
    static void RemoveScriptStepObserver(FDelegateHandle InHandle);
    static FDelegateHandle AddScriptJumpObserver(FScriptJumpObserved::FDelegate InDelegate);
    static void RemoveScriptJumpObserver(FDelegateHandle InHandle);

    // Drops both single-owner handlers and every observer (teardown).
    static void ClearScriptHandlers();

    // Called by the GNatives trampoline. The hook lock serialises the copy of the delegates with
    // registration and teardown; the copies run outside it so a parked stop cannot block teardown. The
    // generation check drops a dispatch whose copy predates a change, which narrows the window but does
    // not close it: a teardown landing after the check still invokes the copy, so an observer must unbind
    // during its owner's teardown.
    static void NotifyScriptStep(const UObject* InContext, const FFrame& InFrame, const UFunction* InFunction, int64 InDispatchAddress);
    static void NotifyScriptJump(const UObject* InContext, const FFrame& InFrame, const UFunction* InFunction, int64 InJumpAddress, int64 InLanding);

#if WITH_DEV_AUTOMATION_TESTS
    // How many dispatch-table entries currently point at the wrapper. Teardown must leave this at 0:
    // an entry that still points here is a jump into the module after it is unloaded.
    static int32 TestCountWrappedEntries();
    static uint32 TestConsumerGeneration();
#endif

private:
    // Hook/wrapper state and the debugging gate. Both are only mutated on the game thread, but the
    // world hooks read the gate from whichever thread ticks, so they are atomics.
    static TAtomic<bool> bInitialized;
    static TAtomic<bool> bEnabled;

    // Attaches or detaches both UWorld hooks in one transaction, with every thread in the process
    // enlisted so none of them can be inside a half-rewritten prologue. False when the transaction did
    // not commit; nothing is left changed in that case.
    static bool RewriteHooks(bool bAttach);

    // Restores the dispatch-table entries this module wrapped and clears the stored originals. Safe on a
    // table it never wrapped, and safe to run after a failed detach.
    static void RestoreNativeTable();

    // Step/jump handlers and observation streams, guarded by HookLock. ActiveConsumers is a lock-free
    // fast-path gate so an idle instrumented build does not take the lock on every dispatched opcode.
    static FOnScriptStep StepHandler;
    static FOnScriptJump JumpHandler;
    static FScriptStepObserved ScriptStepObservers;
    static FScriptJumpObserved ScriptJumpObservers;
    static FCriticalSection HookLock;
    static TAtomic<int32> ActiveConsumers;

    // Bumped by every change to the consumer set. A dispatch reads it under HookLock with its copies and
    // drops them if it changed, so a handler retired by a teardown between the copy and the call is not
    // invoked against its owner.
    static TAtomic<uint32> ConsumersGeneration;

    static void RefreshConsumerCount();

    // Detours replaces the entry point of UWorld::Tick/UWorld::IsPaused with these hooks. On Win64 x64
    // the member-function ABI passes the object in RCX, which is exactly where a static function's
    // first argument lands, so the hooks take the UWorld explicitly instead of reinterpreting a
    // mismatched `this` (that cast is undefined behaviour even though it happens to work on MSVC).
    static void UWorld_Tick(UWorld* World, ELevelTick InTickType, float InSeconds);
    static void (UWorld::*Original_UWorld_Tick)(ELevelTick InTickType, float InSeconds);

    static bool UWorld_IsPaused(const UWorld* World);
    static bool (UWorld::*Original_UWorld_IsPaused)() const;
};

#endif
