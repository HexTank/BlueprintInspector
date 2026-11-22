#include "BlueprintInspectorDetours.h"
#include "BlueprintInspector.h"
#include "BlueprintInspectorPort.h"

#if PLATFORM_WINDOWS
#include "Engine/World.h"
#include "UObject/CoreNative.h"
#if BI_ENGINE_AT_LEAST_5_7
#include "Containers/StaticArray.h"
#endif
// clang-format off: the platform-type pair must enclose the Windows headers.
#include "Windows/AllowWindowsPlatformTypes.h"
#include "detours.h"
#include <tlhelp32.h>
#include "Windows/HideWindowsPlatformTypes.h"
// clang-format on

// 5.7 moved the dispatch table into a TStaticArray and dropped its public declaration, so the
// declaration has to match ScriptCore.cpp to link. 4.25 through 5.6 export the plain array; 5.7
// and 5.8 export the TStaticArray.
#if BI_ENGINE_AT_LEAST_5_7
extern COREUOBJECT_API TStaticArray<FNativeFuncPtr, EX_Max> GNatives;
#else
extern COREUOBJECT_API FNativeFuncPtr GNatives[EX_Max];
#endif

namespace
{
FNativeFuncPtr OriginalGNatives[EX_Max];
TAtomic<bool> bScriptRangeErrorLogged{false};

// Detours rewrites the entry point in place. A thread that is inside the patched function when the
// transaction commits would resume in a half-written prologue, so every thread in the process is
// enlisted, not just the caller. The handles have to stay open until the transaction ends, so the caller
// owns them.
bool EnlistEveryThread(TArray<HANDLE>& OutThreads)
{
    bool bEnlistedAll = true;
    const LONG CurrentThreadResult = DetourUpdateThread(GetCurrentThread());
    if (CurrentThreadResult != NO_ERROR)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Detours: DetourUpdateThread(current thread) failed (%ld)"), CurrentThreadResult);
        bEnlistedAll = false;
    }

    const HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (Snapshot == INVALID_HANDLE_VALUE)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Detours: thread snapshot failed (%lu); the transaction cannot be committed safely"), ::GetLastError());
        return false;
    }

    THREADENTRY32 Entry;
    Entry.dwSize = sizeof(Entry);
    const DWORD ProcessId = ::GetCurrentProcessId();
    const DWORD CurrentThreadId = ::GetCurrentThreadId();
    int32 Enlisted = 0;

    if (Thread32First(Snapshot, &Entry))
    {
        for (;;)
        {
            const bool bIsLiveOtherThread = Entry.th32OwnerProcessID == ProcessId && Entry.th32ThreadID != CurrentThreadId;
            if (bIsLiveOtherThread)
            {
                // 0 for bInheritHandle: the hide-types header undefines the Windows TRUE/FALSE constants.
                const HANDLE Thread = ::OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, 0, Entry.th32ThreadID);
                if (Thread == nullptr)
                {
                    // ERROR_INVALID_PARAMETER means the thread exited between the snapshot and now; there is
                    // nothing left to enlist. Any other failure leaves a live thread unaware of the rewrite.
                    const DWORD Error = ::GetLastError();
                    if (Error != ERROR_INVALID_PARAMETER)
                    {
                        UE_LOG(LogBlueprintInspector, Error, TEXT("Detours: OpenThread(%lu) failed (%lu)"), Entry.th32ThreadID, Error);
                        bEnlistedAll = false;
                    }
                }
                else if (DetourUpdateThread(Thread) == NO_ERROR)
                {
                    OutThreads.Add(Thread);
                    ++Enlisted;
                }
                else
                {
                    UE_LOG(LogBlueprintInspector, Error, TEXT("Detours: DetourUpdateThread(%lu) failed (%lu)"), Entry.th32ThreadID, ::GetLastError());
                    bEnlistedAll = false;
                    ::CloseHandle(Thread);
                }
            }

            if (!Thread32Next(Snapshot, &Entry))
            {
                const DWORD Error = ::GetLastError();
                if (Error != ERROR_NO_MORE_FILES)
                {
                    UE_LOG(LogBlueprintInspector, Error, TEXT("Detours: thread enumeration failed (%lu); the transaction cannot be committed safely"), Error);
                    bEnlistedAll = false;
                }
                break;
            }
        }
    }
    else
    {
        const DWORD Error = ::GetLastError();
        if (Error != ERROR_NO_MORE_FILES)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Detours: thread enumeration failed (%lu); the transaction cannot be committed safely"), Error);
            bEnlistedAll = false;
        }
    }

    ::CloseHandle(Snapshot);
    UE_LOG(LogBlueprintInspector, Verbose, TEXT("Detours: enlisted %d thread(s) besides the caller"), Enlisted);
    return bEnlistedAll;
}

// A copied handler is only executed while its owner is alive. Teardown clears the handler under the lock
// and the copy runs outside it, so a UObject-bound delegate whose object was collected in between must be
// dropped: executing it asserts in development builds.
template <typename TDelegate>
bool IsExecutable(const TDelegate& Delegate)
{
    if (!Delegate.IsBound())
    {
        return false;
    }

    const UObject* const Object = Delegate.GetUObject();
    return Object == nullptr || IsValid(Object);
}

void GNatives_Dispatch(UObject* Context, FFrame& Stack, RESULT_DECL)
{
    if (Stack.Code == nullptr || Stack.Node == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("GNatives dispatch: no code/function"));
        return;
    }

    // The byte read and the offset subtraction both assume Code points just past an opcode inside this
    // function's own script. A script-less/native frame leaves Script empty, and a frame at its first
    // opcode has Code == Script.GetData(), so both would read outside the buffer. Validate first: the
    // delegates below can park the debugger, and a dispatch that is rejected afterwards must not pause.
    const uint8* const ScriptData = Stack.Node->Script.GetData();
    const int32 ScriptSize = Stack.Node->Script.Num();
    if (ScriptData == nullptr || Stack.Code <= ScriptData || Stack.Code > ScriptData + ScriptSize)
    {
        if (!bScriptRangeErrorLogged.Load())
        {
            bScriptRangeErrorLogged.Store(true);
            UE_LOG(LogBlueprintInspector, Error, TEXT("GNatives dispatch: script pointer outside %s's script (size %d); further occurrences are not logged"), *Stack.Node->GetName(), ScriptSize);
        }
        return;
    }

    const uint8 Opcode = *(Stack.Code - 1);
    const int64 DispatchAddress = static_cast<int64>(Stack.Code - ScriptData - 1);

    if (static_cast<int32>(Opcode) >= static_cast<int32>(EX_Max))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("GNatives dispatch: invalid opcode %u"), static_cast<uint32>(Opcode));
        return;
    }

    // Read the slot once: a concurrent restore must not make the null-check and the call disagree.
    const FNativeFuncPtr Original = OriginalGNatives[Opcode];
    if (Original == nullptr || Original == &GNatives_Dispatch)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("GNatives dispatch: invalid opcode %u"), static_cast<uint32>(Opcode));
        return;
    }

    FBlueprintInspectorDetours::NotifyScriptStep(Context, Stack, Stack.Node, DispatchAddress);

    Original(Context, Stack, RESULT_PARAM);

    // A break/continue arm only becomes identifiable once the jump has run: the handler leaves Code on the
    // arm it took. Only the two jump opcodes take the extra branch here.
    if (Opcode == EX_Jump || Opcode == EX_JumpIfNot)
    {
        const int64 Landing = static_cast<int64>(Stack.Code - ScriptData);
        if (Landing >= 0 && Landing <= ScriptSize)
        {
            FBlueprintInspectorDetours::NotifyScriptJump(Context, Stack, Stack.Node, DispatchAddress, Landing);
        }
    }
}
} // namespace

TAtomic<bool> FBlueprintInspectorDetours::bInitialized{false};
TAtomic<bool> FBlueprintInspectorDetours::bEnabled{false};
FBlueprintInspectorDetours::FOnScriptStep FBlueprintInspectorDetours::StepHandler;
FBlueprintInspectorDetours::FOnScriptJump FBlueprintInspectorDetours::JumpHandler;
FBlueprintInspectorDetours::FScriptStepObserved FBlueprintInspectorDetours::ScriptStepObservers;
FBlueprintInspectorDetours::FScriptJumpObserved FBlueprintInspectorDetours::ScriptJumpObservers;
FCriticalSection FBlueprintInspectorDetours::HookLock;
TAtomic<int32> FBlueprintInspectorDetours::ActiveConsumers{0};
TAtomic<uint32> FBlueprintInspectorDetours::ConsumersGeneration{0};
void (UWorld::* FBlueprintInspectorDetours::Original_UWorld_Tick)(ELevelTick InTickType, float InSeconds) = &UWorld::Tick;
bool (UWorld::* FBlueprintInspectorDetours::Original_UWorld_IsPaused)() const = &UWorld::IsPaused;

template <typename T>
struct TIsMemberFunctionPointer
{
    enum
    {
        Value = false
    };
};

template <typename Class, typename Return, typename... Args>
struct TIsMemberFunctionPointer<Return (Class::*)(Args...)>
{
    enum
    {
        Value = true
    };
};

template <typename Class, typename Return, typename... Args>
struct TIsMemberFunctionPointer<Return (Class::*)(Args...) const>
{
    enum
    {
        Value = true
    };
};

template <typename T>
static void* FunctionToVoidPointer(T InValue)
{
    using FnType = typename TRemovePointer<T>::Type;
    static_assert(TIsFunction<FnType>::Value || TIsMemberFunctionPointer<FnType>::Value, "FunctionToVoidPointer takes a function or member function pointer");
    static_assert(sizeof(T) == sizeof(void*), "DetourAttach needs a pointer-sized function pointer");

    // memcpy, not a pointer cast: reading a function pointer through an unrelated type is a strict
    // aliasing violation, and a cast would silently reinterpret the bytes if the sizes ever differed.
    void* Result = nullptr;
    FMemory::Memcpy(static_cast<void*>(&Result), static_cast<const void*>(&InValue), sizeof(Result));
    return Result;
}

bool FBlueprintInspectorDetours::IsInitialized()
{
    return bInitialized.Load();
}

bool FBlueprintInspectorDetours::IsEnabled()
{
    return bEnabled.Load();
}

void FBlueprintInspectorDetours::SetEnabled(const bool bInEnabled)
{
    check(IsInGameThread());

    if (bInEnabled && !IsInitialized())
    {
        // Arming a stop without the wrapper installed means no opcode will reach the hook. Report it and
        // stay disabled instead of publishing a state nothing can observe.
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Detours: debugging requested while the hooks are not installed; staying disabled"));
        return;
    }
    bEnabled.Store(bInEnabled);
}

void FBlueprintInspectorDetours::RefreshConsumerCount()
{
    // Caller holds HookLock.
    int32 Count = 0;
    Count += StepHandler.IsBound() ? 1 : 0;
    Count += JumpHandler.IsBound() ? 1 : 0;
    Count += ScriptStepObservers.IsBound() ? 1 : 0;
    Count += ScriptJumpObservers.IsBound() ? 1 : 0;
    ActiveConsumers.Store(Count);

    // A dispatch that copied the delegates before this change drops them instead of invoking a handler
    // the change just retired.
    ConsumersGeneration.Store(ConsumersGeneration.Load() + 1);
}

void FBlueprintInspectorDetours::UnbindScriptStep()
{
    FScopeLock Lock(&HookLock);
    if (!StepHandler.IsBound())
    {
        return;
    }
    StepHandler.Unbind();
    RefreshConsumerCount();
}

void FBlueprintInspectorDetours::UnbindScriptJump()
{
    FScopeLock Lock(&HookLock);
    if (!JumpHandler.IsBound())
    {
        return;
    }
    JumpHandler.Unbind();
    RefreshConsumerCount();
}

FDelegateHandle FBlueprintInspectorDetours::AddScriptStepObserver(FScriptStepObserved::FDelegate InDelegate)
{
    FScopeLock Lock(&HookLock);
    const FDelegateHandle Handle = ScriptStepObservers.Add(MoveTemp(InDelegate));
    RefreshConsumerCount();
    return Handle;
}

void FBlueprintInspectorDetours::RemoveScriptStepObserver(const FDelegateHandle InHandle)
{
    FScopeLock Lock(&HookLock);
    if (!ScriptStepObservers.Remove(InHandle))
    {
        return;
    }
    RefreshConsumerCount();
}

FDelegateHandle FBlueprintInspectorDetours::AddScriptJumpObserver(FScriptJumpObserved::FDelegate InDelegate)
{
    FScopeLock Lock(&HookLock);
    const FDelegateHandle Handle = ScriptJumpObservers.Add(MoveTemp(InDelegate));
    RefreshConsumerCount();
    return Handle;
}

void FBlueprintInspectorDetours::RemoveScriptJumpObserver(const FDelegateHandle InHandle)
{
    FScopeLock Lock(&HookLock);
    if (!ScriptJumpObservers.Remove(InHandle))
    {
        return;
    }
    RefreshConsumerCount();
}

void FBlueprintInspectorDetours::ClearScriptHandlers()
{
    FScopeLock Lock(&HookLock);
    StepHandler.Unbind();
    JumpHandler.Unbind();
    ScriptStepObservers.Clear();
    ScriptJumpObservers.Clear();
    RefreshConsumerCount();
}

void FBlueprintInspectorDetours::NotifyScriptStep(const UObject* const InContext, const FFrame& InFrame, const UFunction* const InFunction, const int64 InDispatchAddress)
{
    if (ActiveConsumers.Load() == 0)
    {
        return;
    }

    // Copy the delegates, then invoke them without the lock. The handler may park the game thread, and a
    // teardown on another thread must not block behind that stop. The generation read with the copy says
    // whether the set changed before the call: if it did, the copies may name handlers the teardown just
    // retired, so they are dropped.
    FOnScriptStep LocalStep;
    FScriptStepObserved LocalObservers;
    uint32 Generation = 0;
    {
        FScopeLock Lock(&HookLock);
        Generation = ConsumersGeneration.Load();
        LocalStep = StepHandler;
        LocalObservers = ScriptStepObservers;
    }
    if (ConsumersGeneration.Load() != Generation)
    {
        return;
    }

    if (IsExecutable(LocalStep))
    {
        LocalStep.Execute(InContext, InFrame);
    }
    // The handler may have parked the game thread long enough for a teardown to retire the observers.
    if (ConsumersGeneration.Load() != Generation)
    {
        return;
    }
    if (LocalObservers.IsBound())
    {
        LocalObservers.Broadcast(InFunction, InDispatchAddress);
    }
}

void FBlueprintInspectorDetours::NotifyScriptJump(const UObject* const InContext, const FFrame& InFrame, const UFunction* const InFunction, const int64 InJumpAddress, const int64 InLanding)
{
    if (ActiveConsumers.Load() == 0)
    {
        return;
    }

    FOnScriptJump LocalJump;
    FScriptJumpObserved LocalObservers;
    uint32 Generation = 0;
    {
        FScopeLock Lock(&HookLock);
        Generation = ConsumersGeneration.Load();
        LocalJump = JumpHandler;
        LocalObservers = ScriptJumpObservers;
    }
    if (ConsumersGeneration.Load() != Generation)
    {
        return;
    }

    if (IsExecutable(LocalJump))
    {
        LocalJump.Execute(InContext, InFrame, InJumpAddress);
    }
    if (ConsumersGeneration.Load() != Generation)
    {
        return;
    }
    if (LocalObservers.IsBound())
    {
        LocalObservers.Broadcast(InFunction, InJumpAddress, InLanding);
    }
}

void FBlueprintInspectorDetours::Initialize()
{
    check(IsInGameThread());
    if (bInitialized.Load())
    {
        return;
    }

    if (!RewriteHooks(true))
    {
        return;
    }

    // Publish the installed state before any activation: Refresh() refuses to run otherwise, and a
    // request that races this sees a consistent "attached but not yet wrapped".
    bInitialized.Store(true);
}

void FBlueprintInspectorDetours::Activate()
{
    check(IsInGameThread());
    Refresh();
}

void FBlueprintInspectorDetours::Deactivate()
{
    check(IsInGameThread());
    RestoreNativeTable();
}

void FBlueprintInspectorDetours::Deinitialize()
{
    check(IsInGameThread());

    // One teardown path, with no early return. The dispatch table and the delegates point into this
    // module whether or not the detour transaction commits, so a failed detach must not skip them: that
    // is exactly the state where the module is later unloaded with jumps still aimed at it.
    const bool bWasInitialized = bInitialized.Load();
    bool bDetached = false;

    if (bWasInitialized)
    {
        bDetached = RewriteHooks(false);
        if (bDetached)
        {
            // Detours frees the trampolines on detach, so a later Initialize must not hand a freed
            // address to DetourAttach.
            Original_UWorld_Tick = &UWorld::Tick;
            Original_UWorld_IsPaused = &UWorld::IsPaused;
            bInitialized.Store(false);
        }
        else
        {
            // The hooks are still installed. Keep reporting initialized so a later Initialize does not
            // attach them twice; a later Deinitialize retries the detach.
            UE_LOG(LogBlueprintInspector, Error,
                   TEXT("Detours: the UWorld hooks could not be removed; they stay installed and IsInitialized() keeps reporting true. A later Initialize reuses them, a later Deinitialize retries the detach."));
        }
    }

    // Unwrap the dispatch table only when the UWorld hooks actually came down. A failed detach keeps
    // reporting initialized, and a later Initialize returns early, so restoring the table here would
    // leave every consumer believing the dispatcher is installed while opcodes bypass it.
    if (!bWasInitialized || bDetached)
    {
        RestoreNativeTable();
    }

    SetEnabled(false);
}

namespace
{
// Detours writes a plain code pointer into the first slot of the PMF. MSVC stores a non-virtual
// single-inheritance member function as { code pointer, int32 adjustment }, so an adjustment other than
// zero would make the trampoline call the wrong entry. Fail before attaching instead of corrupting it.
bool HasZeroMemberFunctionAdjustment(const void* const Pmf, const size_t Size)
{
    if (Size < sizeof(void*) + sizeof(int32))
    {
        // No separate adjustment slot on this ABI; the plain pointer is the whole representation.
        return true;
    }

    int32 Adjustment = 0;
    FMemory::Memcpy(&Adjustment, static_cast<const uint8*>(Pmf) + sizeof(void*), sizeof(Adjustment));
    return Adjustment == 0;
}
} // namespace

bool FBlueprintInspectorDetours::RewriteHooks(const bool bAttach)
{
    const LONG BeginResult = DetourTransactionBegin();
    if (BeginResult != NO_ERROR)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("DetourTransactionBegin failed: %ld"), BeginResult);
        return false;
    }

    if (!HasZeroMemberFunctionAdjustment(&Original_UWorld_Tick, sizeof(Original_UWorld_Tick)) ||
        !HasZeroMemberFunctionAdjustment(&Original_UWorld_IsPaused, sizeof(Original_UWorld_IsPaused)))
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Detours: a UWorld member-function pointer carries a non-zero adjustment; refusing to attach"));
        const LONG AbortResult = DetourTransactionAbort();
        UE_CLOG(AbortResult != NO_ERROR, LogBlueprintInspector, Error, TEXT("DetourTransactionAbort failed: %ld"), AbortResult);
        return false;
    }

    // Every thread in the process, not just this one: Detours rewrites the prologue in place, and a
    // thread sitting inside UWorld::Tick has to have its instruction pointer relocated with it. Committing
    // with a live thread unaware of the rewrite is the crash this enlistment exists to prevent.
    TArray<HANDLE> Threads;
    if (!EnlistEveryThread(Threads))
    {
        for (const HANDLE Thread : Threads)
        {
            ::CloseHandle(Thread);
        }
        UE_LOG(LogBlueprintInspector, Error, TEXT("Detour %s aborted: not every thread could be enlisted"), bAttach ? TEXT("attach") : TEXT("detach"));
        const LONG AbortResult = DetourTransactionAbort();
        UE_CLOG(AbortResult != NO_ERROR, LogBlueprintInspector, Error, TEXT("DetourTransactionAbort failed: %ld"), AbortResult);
        return false;
    }

    void* const TickDetour = FunctionToVoidPointer(&FBlueprintInspectorDetours::UWorld_Tick);
    void* const PausedDetour = FunctionToVoidPointer(&FBlueprintInspectorDetours::UWorld_IsPaused);

    const LONG TickResult = bAttach
                                ? DetourAttach(reinterpret_cast<void**>(&Original_UWorld_Tick), TickDetour)
                                : DetourDetach(reinterpret_cast<void**>(&Original_UWorld_Tick), TickDetour);
    const LONG PausedResult = TickResult == NO_ERROR
                                  ? (bAttach
                                         ? DetourAttach(reinterpret_cast<void**>(&Original_UWorld_IsPaused), PausedDetour)
                                         : DetourDetach(reinterpret_cast<void**>(&Original_UWorld_IsPaused), PausedDetour))
                                  : TickResult;

    bool bCommitted = false;
    if (TickResult != NO_ERROR || PausedResult != NO_ERROR)
    {
        if (TickResult != NO_ERROR)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("UWorld detour %s failed: Tick=%ld"), bAttach ? TEXT("attach") : TEXT("detach"), TickResult);
        }
        else
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("UWorld detour %s failed: IsPaused=%ld"), bAttach ? TEXT("attach") : TEXT("detach"), PausedResult);
        }
        const LONG AbortResult = DetourTransactionAbort();
        UE_CLOG(AbortResult != NO_ERROR, LogBlueprintInspector, Error, TEXT("DetourTransactionAbort failed: %ld"), AbortResult);
        if (bAttach)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("Blueprint breakpoints and pause are unavailable in this session until the UWorld hooks attach; close any Live Coding session or restart the editor if the failure repeats."));
            // A partially attached transaction may already have installed trampolines that the abort frees.
            // A failed detach keeps its detour and its originals: those are needed to retry the detach.
            Original_UWorld_Tick = &UWorld::Tick;
            Original_UWorld_IsPaused = &UWorld::IsPaused;
        }
    }
    else
    {
        const LONG CommitResult = DetourTransactionCommit();
        if (CommitResult != NO_ERROR)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("DetourTransactionCommit failed: %ld"), CommitResult);
            DetourTransactionAbort();
            if (bAttach)
            {
                Original_UWorld_Tick = &UWorld::Tick;
                Original_UWorld_IsPaused = &UWorld::IsPaused;
            }
        }
        else
        {
            bCommitted = true;
        }
    }

    // The enlisted handles only have to outlive the transaction.
    for (const HANDLE Thread : Threads)
    {
        ::CloseHandle(Thread);
    }

    return bCommitted;
}

void FBlueprintInspectorDetours::RestoreNativeTable()
{
    FScopeLock Lock(&HookLock);

    // Only slots Refresh actually wrapped are touched, so an unimplemented (null) opcode stays null
    // rather than being pointed at this module's dispatcher after teardown. The original handler is left
    // in the slot: a thread already inside GNatives_Dispatch may read it after the table is restored,
    // and a null there makes it return without consuming the opcode's operands.
    for (int32 Index = 0; Index < EX_Max; ++Index)
    {
        if (GNatives[Index] == &GNatives_Dispatch)
        {
            GNatives[Index] = OriginalGNatives[Index];
        }
    }
}

#if WITH_DEV_AUTOMATION_TESTS
int32 FBlueprintInspectorDetours::TestCountWrappedEntries()
{
    int32 Wrapped = 0;
    for (int32 Index = 0; Index < EX_Max; ++Index)
    {
        Wrapped += GNatives[Index] == &GNatives_Dispatch ? 1 : 0;
    }
    return Wrapped;
}

uint32 FBlueprintInspectorDetours::TestConsumerGeneration()
{
    return ConsumersGeneration.Load();
}
#endif

void FBlueprintInspectorDetours::Refresh()
{
    check(IsInGameThread());

    if (!bInitialized.Load())
    {
        // A failed Initialize leaves the hooks detached. Retry here so a later breakpoint request can
        // recover once the address space allows the trampolines, for example after Live Coding closes.
        // Nothing was wrapped on the failed path, so this cannot leave the table half-installed.
        if (!RewriteHooks(true))
        {
            return;
        }
        bInitialized.Store(true);
    }

    // Serialize the table mutation with teardown and with the consumer bookkeeping, so a restore cannot
    // run between the original copy and the dispatcher store.
    FScopeLock Lock(&HookLock);

    // Wrap only real handlers. A null slot has no original to call, and wrapping it makes
    // GNatives_Dispatch report an invalid opcode on every dispatch and return without consuming the
    // opcode's operands, which desynchronises the rest of the byte stream.
    int32 Rewrapped = 0;
    int32 Empty = 0;
    for (int32 Index = 0; Index < EX_Max; ++Index)
    {
        if (GNatives[Index] == &GNatives_Dispatch)
        {
            continue;
        }
        if (GNatives[Index] == nullptr)
        {
            ++Empty;
            continue;
        }
        OriginalGNatives[Index] = GNatives[Index];
        GNatives[Index] = &GNatives_Dispatch;
        ++Rewrapped;
    }

    UE_CLOG(Rewrapped > 0 || Empty > 0, LogBlueprintInspector, Log, TEXT("GNatives opcode dispatch wrapped (%d entries, %d unimplemented)"), Rewrapped, Empty);
}

void FBlueprintInspectorDetours::UWorld_Tick(UWorld* const World, const ELevelTick InTickType, const float InSeconds)
{
    if (IsEnabled())
    {
        return;
    }

    if (World == nullptr)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("UWorld::Tick detour received a null world"));
        return;
    }

    (World->*Original_UWorld_Tick)(InTickType, InSeconds);
}

bool FBlueprintInspectorDetours::UWorld_IsPaused(const UWorld* const World)
{
    if (IsEnabled())
    {
        // The debugger's own paused state: keep the engine paused while a stop is being served.
        return true;
    }

    if (World == nullptr)
    {
        // A null world cannot report its own pause state; claim paused so callers skip work they cannot run.
        UE_LOG(LogBlueprintInspector, Warning, TEXT("UWorld::IsPaused detour received a null world"));
        return true;
    }

    return (World->*Original_UWorld_IsPaused)();
}

#endif
