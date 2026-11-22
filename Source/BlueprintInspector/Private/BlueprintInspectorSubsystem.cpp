#include "BlueprintInspectorSubsystem.h"

#include "BlueprintAudit.h"
#include "BlueprintExpression.h"
#include "BlueprintInspector.h"
#include "BlueprintInspectorDetours.h"
#include "BlueprintInspectorPort.h"
#include "BlueprintInspectorVariableEdit.h"
#include "BlueprintScriptDisassembler.h"
#include "BlueprintScriptVisualizer_Pseudo.h"

#include "Application/SlateWindowHelper.h"
#include "Async/Async.h"
#include "Async/Future.h"
#include "Engine/Blueprint.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/Event.h"
#include "HAL/PlatformProcess.h"
#include "Layout/WidgetPath.h"
#include "Templates/Atomic.h"
#include "UObject/FieldIterator.h"
#include "UObject/Package.h"
#include "UObject/Script.h"
#include "Widgets/SViewport.h"
#include "Widgets/SWindow.h"

// FBlueprintExceptionInfo lives in UObject/Script.h up to 4.27 and in its own header from 5.0 on.
#if __has_include("Blueprint/BlueprintExceptionInfo.h")
#include "Blueprint/BlueprintExceptionInfo.h"
#endif

namespace
{
// A game-thread request must not deadlock against the parked frame it needs serviced; the same applies
// to the slice the parked loop uses to check for a disconnect.
constexpr double GameThreadRequestTimeoutSeconds = 5.0;
constexpr int32 ParkedWaitSliceMilliseconds = 200;
} // namespace

#if PLATFORM_WINDOWS
#include "Windows/WindowsHWrapper.h"
#endif

namespace
{
template <typename TResult>
TGameThreadCallResult<TResult> RunOnGameThread(TUniqueFunction<TResult()> InFunction)
{
    if (IsInGameThread())
    {
        return {InFunction(), false};
    }

    TPromise<TResult> Promise;
    TFuture<TResult> Future = Promise.GetFuture();
    // A timeout abandons the wait, not the queued work. The flag lets the queued lambda skip a mutating
    // callback after the caller has already been told it failed.
    TSharedPtr<TAtomic<bool>> Cancelled = MakeShared<TAtomic<bool>>(false);
    AsyncTask(ENamedThreads::GameThread, [Cancelled, Promise = MoveTemp(Promise), Function = MoveTemp(InFunction)]() mutable {
        if (Cancelled->Load())
        {
            Promise.SetValue(TResult());
            return;
        }
        Promise.SetValue(Function());
    });

    // DAP dispatch threads call this while the game thread may be parked in OnScriptStep waiting for
    // `continue`. An unbounded wait would deadlock the request thread against the very frame it needs
    // to service, so bound it and let the handler report the timeout instead of an empty answer.
    const FTimespan Timeout = FTimespan::FromSeconds(GameThreadRequestTimeoutSeconds);
    if (!Future.WaitFor(Timeout))
    {
        Cancelled->Store(true);
        UE_LOG(LogBlueprintInspector, Error, TEXT("RunOnGameThread timed out after %.0f s; the game thread is not servicing game-thread work"), Timeout.GetTotalSeconds());
        return {TResult(), true};
    }

    return {Future.Get(), false};
}

// A DAP request arrives on a pool thread while a breakpoint may have the game thread parked. The two
// serving orders are separate entry points so a call site states which one it needs.
//
// Cache-first: prepared state wins whenever it exists; while the VM is parked a miss returns the
// default instead of marshalling.
template <typename TResult, typename TCached, typename TGameThread>
TGameThreadCallResult<TResult> ServeFromCacheOrMarshal(const TWeakObjectPtr<UBlueprintInspectorSubsystem>& WeakThis, TCached Cached, TGameThread GameThreadWork)
{
    if (UBlueprintInspectorSubsystem* const Self = WeakThis.Get())
    {
        if (TOptional<TResult> Prepared = Cached(*Self))
        {
            return {MoveTemp(Prepared.GetValue()), false};
        }

        if (Self->IsPaused())
        {
            return {TResult(), false, true};
        }
    }

    return RunOnGameThread<TResult>([WeakThis, GameThreadWork = MoveTemp(GameThreadWork)]() -> TResult {
		if (UBlueprintInspectorSubsystem* const Self = WeakThis.Get())
		{
			return GameThreadWork(*Self);
		}
		return TResult(); });
}

// Pause-aware: prepared state is only consulted while the VM is parked; otherwise the work marshals.
// The pause state is re-read after the cache lookup so a resume that raced it still marshals.
template <typename TResult, typename TCached, typename TGameThread>
TGameThreadCallResult<TResult> MarshalUnlessPaused(const TWeakObjectPtr<UBlueprintInspectorSubsystem>& WeakThis, const TCached& Cached, TGameThread GameThreadWork)
{
    if (UBlueprintInspectorSubsystem* const Self = WeakThis.Get())
    {
        if (Self->IsPaused())
        {
            if (TOptional<TResult> Prepared = Cached(*Self))
            {
                return {MoveTemp(Prepared.GetValue()), false};
            }
            if (Self->IsPaused())
            {
                return {TResult(), false, true};
            }
        }
    }

    return RunOnGameThread<TResult>([WeakThis, GameThreadWork = MoveTemp(GameThreadWork)]() -> TResult {
		if (UBlueprintInspectorSubsystem* const Self = WeakThis.Get())
		{
			return GameThreadWork(*Self);
		}
		return TResult(); });
}

// The dispatch hook runs after FFrame::Step consumes the opcode byte, so Code - 1 is the opcode
// offset. That is the address the source map records and ToggleBreakpoint arms. It matches the
// engine's OnScriptException convention. A frame whose Code is outside the script is not addressable:
// subtracting a foreign pointer would report a line the function never had.
bool TryGetScriptOffset(const FFrame& InFrame, int64& OutOffset)
{
    if (InFrame.Node == nullptr || InFrame.Code == nullptr || InFrame.Node->Script.Num() == 0)
    {
        return false;
    }

    const uint8* const Start = InFrame.Node->Script.GetData();
    const uint8* const End = Start + InFrame.Node->Script.Num();
    if (InFrame.Code < Start || InFrame.Code > End)
    {
        return false;
    }

    OutOffset = static_cast<int64>(InFrame.Code - Start);
    return true;
}

int64 GetStatementAddress(const FFrame& InFrame)
{
    int64 Offset = 0;
    if (!TryGetScriptOffset(InFrame, Offset) || Offset == 0)
    {
        return INDEX_NONE;
    }
    return Offset - 1;
}

// Which client filter covers a VM exception. Kinds this engine does not know (a newer release may add
// one) fall into RuntimeError, so a new error is still reachable; the debugger kinds are not errors.
EBlueprintExceptionFilter GetExceptionFilter(const EBlueprintExceptionType::Type Type)
{
    switch (Type)
    {
    case EBlueprintExceptionType::Breakpoint:
    case EBlueprintExceptionType::Tracepoint:
    case EBlueprintExceptionType::WireTracepoint:
        return EBlueprintExceptionFilter::None;
    case EBlueprintExceptionType::AccessViolation:
        return EBlueprintExceptionFilter::AccessedNone;
    case EBlueprintExceptionType::InfiniteLoop:
        return EBlueprintExceptionFilter::InfiniteLoop;
    default:
        return EBlueprintExceptionFilter::RuntimeError;
    }
}

int32 GetFrameDepth(const FFrame& InFrame)
{
    int32 Depth = 0;
    for (const FFrame* Frame = &InFrame; Frame->PreviousFrame; Frame = Frame->PreviousFrame)
    {
        ++Depth;
    }
    return Depth;
}

} // namespace

FCriticalSection UBlueprintInspectorSubsystem::ReferenceOwnerLock;
TWeakObjectPtr<UObject> UBlueprintInspectorSubsystem::ReferenceOwner;

TArray<FFunctionProperties> UBlueprintInspectorSubsystem::AcquireClassList()
{
    return ClassRegistry.AcquireClassList(GetPIEPackage());
}

const UPackage* UBlueprintInspectorSubsystem::GetPIEPackage() const
{
    const UWorld* const World = GetGameInstance() ? GetGameInstance()->GetWorld() : nullptr;
    return (World && World->IsPlayInEditor()) ? World->GetOutermost() : nullptr;
}

#if WITH_DEV_AUTOMATION_TESTS
bool UBlueprintInspectorSubsystem::TestEnsureBreakpoint(const UFunction* const Function, const int64 Address)
{
    if (Function == nullptr)
    {
        return false;
    }
    if (!BreakpointStore.IsSet(Function, Address) && BreakpointStore.Toggle(Function, Address) != EBreakpointToggle::Set)
    {
        return false;
    }
#if PLATFORM_WINDOWS
    FBlueprintInspectorDetours::Refresh();
#endif
    return BreakpointStore.IsSet(Function, Address);
}

bool UBlueprintInspectorSubsystem::TestMarshalUnlessPaused(const bool bResumeDuringCache, int32& OutValue)
{
    Pause.PublishPaused();
    const TGameThreadCallResult<int32> Result = MarshalUnlessPaused<int32>(TWeakObjectPtr<UBlueprintInspectorSubsystem>(this), [this, bResumeDuringCache](UBlueprintInspectorSubsystem&) -> TOptional<int32> {
        if (bResumeDuringCache)
        {
            Pause.EndPause();
        }
        return TOptional<int32>(); }, [](UBlueprintInspectorSubsystem&) { return 42; });
    OutValue = Result.Value;
    return !Result.bTimedOut && !Result.bUnavailable;
}

bool UBlueprintInspectorSubsystem::TestClearBreakpoint(const UFunction* const Function, const int64 Address)
{
    if (Function != nullptr && BreakpointStore.IsSet(Function, Address))
    {
        BreakpointStore.Toggle(Function, Address);
    }
#if PLATFORM_WINDOWS
    FBlueprintInspectorDetours::Refresh();
#endif
    return !BreakpointStore.IsSet(Function, Address);
}

bool UBlueprintInspectorSubsystem::IsReferenceOwnerForTest() const
{
    FScopeLock Lock(&ReferenceOwnerLock);
    return ReferenceOwner.Get() == this;
}

bool UBlueprintInspectorSubsystem::HasReferenceOwnerForTest()
{
    FScopeLock Lock(&ReferenceOwnerLock);
    return ReferenceOwner.IsValid();
}

bool UBlueprintInspectorSubsystem::BindScriptHandlersForTest(UBlueprintInspectorSubsystem* const Owner)
{
    return FBlueprintInspectorDetours::BindScriptStep(Owner, &UBlueprintInspectorSubsystem::OnScriptStep) && FBlueprintInspectorDetours::BindScriptJump(Owner, &UBlueprintInspectorSubsystem::OnScriptJump);
}

void UBlueprintInspectorSubsystem::ClearReferenceOwnerForTest(UBlueprintInspectorSubsystem* const Owner)
{
    FScopeLock Lock(&ReferenceOwnerLock);
    if (ReferenceOwner.Get() == Owner)
    {
        ReferenceOwner.Reset();
    }
}
#endif

void UBlueprintInspectorSubsystem::ApplyPendingBreakpointRequests()
{
    // The pause is already over: EndPause() cleared the flag and took the write queue under the same
    // lock, so a request either queued here or took the normal game-thread path.
    const TArray<FBlueprintPendingBreakpoints> Pending = Pause.TakePendingBreakpoints();

    for (const FBlueprintPendingBreakpoints& Request : Pending)
    {
        BreakpointStore.Apply(Request.Identity, Request.Requests, GetPIEPackage());
    }
}

void UBlueprintInspectorSubsystem::ReleaseReferenceOwner()
{
    FScopeLock Lock(&ReferenceOwnerLock);
    if (ReferenceOwner.Get() == this)
    {
        ReferenceOwner.Reset();
    }
}

void UBlueprintInspectorSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
    Super::Initialize(Collection);

#if PLATFORM_WINDOWS
    bool bIsReferenceOwner = false;
    {
        // A lock, not TAtomic<TWeakObjectPtr>: the weak pointer is not trivially copyable, so an atomic
        // wrapper copies it non-atomically. The lock only guards subsystem start/stop.
        FScopeLock Lock(&ReferenceOwnerLock);
        if (!ReferenceOwner.IsValid())
        {
            ReferenceOwner = this;
            bIsReferenceOwner = true;
        }
    }
    if (bIsReferenceOwner)
    {
        Pause.CreatePauseEvent();
        BlueprintInspectorDap = FUBlueprintInspectorDap::Create();

        // A handle retarget drops the stale render and re-points the breakpoint records at the class's
        // current functions. The registry calls this on the game thread while holding its handle lock,
        // so the handler must not take a registry lock - RebindClass does not.
        ClassRegistry.SetRetargetHandler([this](const UClass* Class) { BreakpointStore.RebindClass(Class); });

        // The callbacks outlive this subsystem when a request is still in flight, so they capture a
        // weak object pointer and check it before touching the subsystem.
        TWeakObjectPtr<UBlueprintInspectorSubsystem> WeakThis(this);
        FBlueprintInspectorDapCallbacks Callbacks;

        Callbacks.AcquireClassList = [WeakThis]() -> TGameThreadCallResult<TArray<FFunctionProperties>> {
            return MarshalUnlessPaused<TArray<FFunctionProperties>>(WeakThis, [](UBlueprintInspectorSubsystem& Self) {
                    TArray<FFunctionProperties> Cached;
                    Self.ClassRegistry.CopyCachedClassList(Cached);
                    return TOptional<TArray<FFunctionProperties>>(MoveTemp(Cached)); }, [](UBlueprintInspectorSubsystem& Self) { return Self.AcquireClassList(); });
        };

        Callbacks.AcquireClassFunctions = [WeakThis](const int32 ID) -> TGameThreadCallResult<FString> {
            return ServeFromCacheOrMarshal<FString>(WeakThis, [ID](UBlueprintInspectorSubsystem& Self) {
                    FString Cached;
                    if (Self.ClassRegistry.TryCopySource(ID, Cached))
                    {
                        return TOptional<FString>(MoveTemp(Cached));
                    }

                    if (Self.IsPaused())
                    {
                        UE_LOG(LogBlueprintInspector, Warning, TEXT("Source %d was not rendered before the breakpoint; returning no source"), ID);
                    }
                    return TOptional<FString>(); }, [ID](UBlueprintInspectorSubsystem& Self) { return Self.ClassRegistry.AcquireClassFunctions(ID); });
        };

        Callbacks.AcquireSourceMap = [WeakThis](const FBlueprintSourceIdentity& Identity) -> TGameThreadCallResult<FBlueprintSourceMap> {
            return MarshalUnlessPaused<FBlueprintSourceMap>(WeakThis, [Identity](UBlueprintInspectorSubsystem& Self) {
                    FBlueprintSourceMap Cached;
                    if (Self.ClassRegistry.TryCopySourceMap(Identity, Cached))
                    {
                        return TOptional<FBlueprintSourceMap>(MoveTemp(Cached));
                    }

                    if (Self.IsPaused())
                    {
                        UE_LOG(LogBlueprintInspector, Warning, TEXT("Source map requested for '%s' while paused, but it has no cached render"), *Identity.ClassName);
                    }
                    return TOptional<FBlueprintSourceMap>(); }, [Identity](UBlueprintInspectorSubsystem& Self) {
                    const TOptional<FBlueprintRenderedSource> Rendered = Self.ClassRegistry.EnsureClassSourceFor(Identity, Self.GetPIEPackage());
                    return Rendered.IsSet() ? Rendered->Map : FBlueprintSourceMap(); });
        };

        Callbacks.ApplyBreakpoints = [WeakThis](const FBlueprintSourceIdentity& Identity, const TArray<FBlueprintBreakpointRequest>& Requests) -> TGameThreadCallResult<TArray<FBlueprintBreakpointResolution>> {
            return MarshalUnlessPaused<TArray<FBlueprintBreakpointResolution>>(WeakThis, [Identity, Requests](UBlueprintInspectorSubsystem& Self) -> TOptional<TArray<FBlueprintBreakpointResolution>> {
                    // Breakpoint state is game-thread-only. The resume clears the flag and takes this
                    // queue under the same lock, and applies it before the VM runs another opcode.
                    if (!Self.Pause.QueueBreakpointsIfPaused({Identity, Requests}))
                    {
                        // Resumed between the wrapper's check and this call; let the game-thread path run.
                        return TOptional<TArray<FBlueprintBreakpointResolution>>();
                    }
                    return Self.BreakpointStore.ResolveCached(Identity, Requests); }, [Identity, Requests](UBlueprintInspectorSubsystem& Self) { return Self.BreakpointStore.Apply(Identity, Requests, Self.GetPIEPackage()); });
        };

        // The DAP marks breakpoints unverified when the VM hooks are not installed, instead of letting the
        // client believe a breakpoint will hit.
        Callbacks.IsPauseAvailable = []() { return FBlueprintInspectorDetours::IsInitialized(); };

        Callbacks.AcquireStackFrames = [WeakThis](const int32) -> TArray<FBlueprintStackFrame> {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->Pause.AcquireStackFrames();
            }
            return TArray<FBlueprintStackFrame>();
        };

        Callbacks.AcquireScopes = [WeakThis](const int32 FrameId) -> TArray<FBlueprintScope> {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->Pause.AcquireScopes(FrameId);
            }
            return TArray<FBlueprintScope>();
        };

        Callbacks.AcquireVariables = [WeakThis](const int32 VariablesReference) -> TArray<FBlueprintVariable> {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->Pause.AcquireVariables(VariablesReference);
            }
            return TArray<FBlueprintVariable>();
        };

        Callbacks.SetVariable = [WeakThis](const int32 VariablesReference, const FString& Name, const FString& Value) -> FBlueprintSetVariableResult {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->Pause.SetVariable(VariablesReference, Name, Value);
            }
            FBlueprintSetVariableResult Result;
            Result.Error = TEXT("BlueprintInspector is shutting down.");
            return Result;
        };

        Callbacks.EvaluateExpression = [WeakThis](const int32 FrameId, const FString& Expression) -> FBlueprintEvaluateResult {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->Pause.EvaluateExpression(FrameId, Expression);
            }
            FBlueprintEvaluateResult Result;
            Result.Error = TEXT("BlueprintInspector is shutting down.");
            return Result;
        };

        Callbacks.ContinueExecution = [WeakThis]() {
            if (UBlueprintInspectorSubsystem* const Self = WeakThis.Get())
            {
                const TSharedPtr<FEvent, ESPMode::ThreadSafe> Event = Self->Pause.GetEvent();
                Self->StepState.Resume();

                // Drop the paused snapshot before releasing the VM, so nothing can read frames whose
                // stack has already been resumed.
                Self->Pause.ClearFrame();

                if (Event.IsValid())
                {
                    Event->Trigger();
                }

                // A disconnect can arrive while execution is running (a step left the pointer yielded
                // and no stop followed). There is no parked game thread to recapture on, so hand the
                // pointer back the next time the game thread runs. A parked resume recaptures in
                // OnScriptStep itself, so this is only queued when nothing is stopped.
                if (!Self->Pause.IsPausedTakingRequestLock())
                {
                    AsyncTask(ENamedThreads::GameThread, [WeakThis]() {
                        if (UBlueprintInspectorSubsystem* const Self = WeakThis.Get())
                        {
                            Self->SetInputCaptureReleased(false);
                        }
                    });
                }
            }
        };

        Callbacks.StepExecution = [WeakThis](const EBlueprintStepKind Kind) -> bool {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->RequestStep(Kind);
            }
            return false;
        };

        Callbacks.StartCapture = [WeakThis]() -> FBlueprintHotCaptureStatus {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                const FBlueprintHotCaptureStatus Status = Self->HotCapture.Start();

                // Warm the metadata on the game thread so the first snapshot of a run is labelled
                // immediately. A start while the game thread is parked leaves the cache empty; the
                // first request then fills it when the game thread runs again.
                AsyncTask(ENamedThreads::GameThread, [WeakThis]() {
                    if (UBlueprintInspectorSubsystem* const Inner = WeakThis.Get())
                    {
                        Inner->HotCapture.AcquireMetadata();
                    }
                });
                return Status;
            }
            return FBlueprintHotCaptureStatus();
        };

        Callbacks.StopCapture = [WeakThis]() -> FBlueprintHotCaptureStatus {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->HotCapture.Stop();
            }
            return FBlueprintHotCaptureStatus();
        };

        Callbacks.ResetCapture = [WeakThis]() -> FBlueprintHotCaptureStatus {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->HotCapture.Reset();
            }
            return FBlueprintHotCaptureStatus();
        };

        Callbacks.QueryCapture = [WeakThis]() -> FBlueprintHotCaptureStatus {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                return Self->HotCapture.Query();
            }
            return FBlueprintHotCaptureStatus();
        };

        Callbacks.ClearCapture = [WeakThis]() -> FBlueprintHotCaptureStatus {
            if (UBlueprintInspectorSubsystem* Self = WeakThis.Get())
            {
                Self->HotCapture.Clear();
                // The filters are the client's; with no client there is nobody to stop for.
                Self->SetExceptionFilters(FExceptionFilterSet());
            }
            return FBlueprintHotCaptureStatus();
        };

        // Both capture reports walk the class handles and the source maps, which hold UObject weak
        // pointers, so they run on the game thread. While the VM is parked that work cannot run, and the
        // helper returns an empty result rather than leave the request waiting on a stopped thread.
        Callbacks.AcquireHotLines = [WeakThis](const int32 MaxLines) -> TGameThreadCallResult<FBlueprintHotLinesResult> {
            return ServeFromCacheOrMarshal<FBlueprintHotLinesResult>(WeakThis, [](UBlueprintInspectorSubsystem&) { return TOptional<FBlueprintHotLinesResult>(); }, [MaxLines](UBlueprintInspectorSubsystem& Self) { return Self.HotCapture.AcquireHotLines(MaxLines); });
        };

        Callbacks.AcquireCaptureMetadata = [WeakThis]() -> TGameThreadCallResult<FBlueprintCaptureMetadata> {
            return ServeFromCacheOrMarshal<FBlueprintCaptureMetadata>(WeakThis, [](UBlueprintInspectorSubsystem& Self) {
                    FBlueprintCaptureMetadata Cached;
                    return Self.HotCapture.TryCopyMetadata(Cached) ? TOptional<FBlueprintCaptureMetadata>(MoveTemp(Cached)) : TOptional<FBlueprintCaptureMetadata>(); }, [](UBlueprintInspectorSubsystem& Self) { return Self.HotCapture.AcquireMetadata(); });
        };

        Callbacks.AcquireCoverage = [WeakThis](const int32 MaxLines) -> TGameThreadCallResult<FBlueprintCoverageResult> {
            return ServeFromCacheOrMarshal<FBlueprintCoverageResult>(WeakThis, [](UBlueprintInspectorSubsystem&) { return TOptional<FBlueprintCoverageResult>(); }, [MaxLines](UBlueprintInspectorSubsystem& Self) { return Self.HotCapture.AcquireCoverage(MaxLines); });
        };

        Callbacks.ApplyExceptionFilters = [WeakThis](const TArray<FString>& FilterNames) {
            if (UBlueprintInspectorSubsystem* const Self = WeakThis.Get())
            {
                FExceptionFilterSet Filters;
                for (const FString& FilterName : FilterNames)
                {
                    if (!Filters.AddByName(FilterName))
                    {
                        // A client may offer a filter this build does not know; it simply never matches.
                        UE_LOG(LogBlueprintInspector, Warning, TEXT("Unknown exception filter '%s'"), *FilterName);
                    }
                }
                Self->SetExceptionFilters(Filters);
            }
        };

        Callbacks.RunAudit = [WeakThis](const int32 MaxFindings) -> TGameThreadCallResult<FBlueprintAuditResult> {
            return MarshalUnlessPaused<FBlueprintAuditResult>(WeakThis, [](UBlueprintInspectorSubsystem&) { return TOptional<FBlueprintAuditResult>(); }, [MaxFindings](UBlueprintInspectorSubsystem& Self) { return Self.RunAudit(MaxFindings); });
        };

        BlueprintInspectorDap->SetCallbacks(Callbacks);

        // The VM broadcasts script errors on the game thread; the handler decides whether one parks.
        ScriptExceptionHandle = FBlueprintCoreDelegates::OnScriptException.AddUObject(this, &UBlueprintInspectorSubsystem::OnScriptException);

        const bool bStepBound = FBlueprintInspectorDetours::BindScriptStep(this, &UBlueprintInspectorSubsystem::OnScriptStep);
        const bool bJumpBound = bStepBound && FBlueprintInspectorDetours::BindScriptJump(this, &UBlueprintInspectorSubsystem::OnScriptJump);
        if (!bStepBound || !bJumpBound)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("BlueprintInspector could not bind the script handlers; the streams are already owned"));
            if (bStepBound)
            {
                FBlueprintInspectorDetours::UnbindScriptStep();
            }
            FBlueprintCoreDelegates::OnScriptException.Remove(ScriptExceptionHandle);
            ScriptExceptionHandle.Reset();
            BlueprintInspectorDap.Reset();
            ReleaseReferenceOwner();
            return;
        }
        FBlueprintInspectorDetours::Initialize();
        FBlueprintInspectorDetours::Activate();
        if (!BlueprintInspectorDap->StartSession())
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("BlueprintInspector could not start its DAP listener"));
            FBlueprintInspectorDetours::UnbindScriptStep();
            FBlueprintInspectorDetours::UnbindScriptJump();
            FBlueprintInspectorDetours::Deactivate();
            FBlueprintInspectorDetours::Deinitialize();
            FBlueprintCoreDelegates::OnScriptException.Remove(ScriptExceptionHandle);
            ScriptExceptionHandle.Reset();
            BlueprintInspectorDap.Reset();
            ReleaseReferenceOwner();
        }
    }
#endif
}

void UBlueprintInspectorSubsystem::Deinitialize()
{
#if PLATFORM_WINDOWS
    bool bIsReferenceOwner = false;
    {
        FScopeLock Lock(&ReferenceOwnerLock);
        bIsReferenceOwner = ReferenceOwner.Get() == this;
        if (bIsReferenceOwner)
        {
            ReferenceOwner.Reset();
        }
    }
    if (bIsReferenceOwner)
    {
        if (BlueprintInspectorDap)
        {
            BlueprintInspectorDap->EndSession();
            BlueprintInspectorDap->ClearCallbacks();
            BlueprintInspectorDap.Reset();
        }

        // Drop the member under the lock, then release any waiter from a copy. Deinitialize runs on the
        // game thread, so the game thread itself is not parked here; a DAP thread holding a copy keeps
        // the event alive until it returns it to the pool.
        const TSharedPtr<FEvent, ESPMode::ThreadSafe> Event = Pause.TakeEvent();
        StepState.Reset();
        // End the pause as one transition: a write that arrives now is rejected instead of queued for a
        // game thread that will not drain again.
        Pause.EndPause();
        Pause.DropPendingBreakpoints();
        if (Event.IsValid())
        {
            Event->Trigger();
        }

        // Deinitialize can run while a paused frame is still published (PIE shutdown). Drop the frame
        // and its variable groups so no later request can read pointers into the old VM stack.
        Pause.ClearFrame();

        if (ScriptExceptionHandle.IsValid())
        {
            FBlueprintCoreDelegates::OnScriptException.Remove(ScriptExceptionHandle);
            ScriptExceptionHandle.Reset();
        }

        FBlueprintInspectorDetours::Deactivate();
        FBlueprintInspectorDetours::Deinitialize();
        FBlueprintInspectorDetours::ClearScriptHandlers();
        HotCapture.Clear();
        BreakpointStore.Clear();
    }
#endif
    Super::Deinitialize();
}

bool UBlueprintInspectorSubsystem::RequestStep(const EBlueprintStepKind Kind)
{
    const TSharedPtr<FEvent, ESPMode::ThreadSafe> Event = Pause.GetEvent();

    if (!Pause.IsPaused() || !Event.IsValid())
    {
        return false;
    }

    // A step-out from the root frame has no caller to stop in. TryArmStep applies that rule and records
    // the new mode; nothing is armed when it returns false.
    if (!StepState.TryArmStep(Kind))
    {
        return false;
    }

    // The published snapshot belongs to the stop that just ended. End the pause as one transition, then
    // clear the snapshot: a write that arrives after the flag clears is rejected, so none is left for a
    // loop that has stopped draining, and none can read frames whose stack has already resumed.
    Pause.EndPause();
    Pause.ClearFrame();

    // A step is still a debug stop: keep the pointer yielded while the VM runs to the next statement.
    Event->Trigger();
    return true;
}

void UBlueprintInspectorSubsystem::SetInputCaptureReleased(const bool bReleased)
{
    if (bReleased == bInputCaptureReleased)
    {
        return;
    }

    if (!FSlateApplication::IsInitialized())
    {
        return;
    }

    FSlateApplication& Slate = FSlateApplication::Get();
    TSharedPtr<SViewport> GameViewport = Slate.GetGameViewport();
    if (!GameViewport.IsValid())
    {
        return;
    }

    if (bReleased)
    {
        // Shows the cursor, releases Slate's pointer capture and unlocks (unclips) the cursor, so it can
        // leave the game window while the game thread is parked. No Slate tick is pumped.
        bInputCaptureReleased = true;
        Slate.ResetToDefaultPointerInputSettings();
        return;
    }

    TSharedPtr<SWindow> Window = Slate.FindWidgetWindow(GameViewport.ToSharedRef());
    if (!Window.IsValid() || !Window->GetNativeWindow().IsValid())
    {
        return;
    }

    // The stop gives the debugger focus, so the game window may be behind it. Ask for the foreground
    // back; whether the OS grants it is the user's call, and a refusal leaves the pointer free for the
    // debugger instead of capturing a background window.
    Window->HACK_ForceToFront();

#if PLATFORM_WINDOWS
    void* const WindowHandle = Window->GetNativeWindow()->GetOSWindowHandle();
    if (WindowHandle == nullptr || ::GetForegroundWindow() != static_cast<HWND>(WindowHandle))
    {
        // The debugger kept the foreground. The click that returns to the game reaches the viewport and
        // takes the capture through the normal mouse-down path, so leave the pointer alone here.
        return;
    }

    // Take the pointer back the way a click does. Activating the viewport only captures when the
    // platform's activation rules call for it, and an editor (PIE) viewport deliberately does not; a
    // mouse-caused focus event asks the viewport's own focus path for the capture in both cases. This
    // also avoids fighting a deactivation that the parked game thread has not pumped yet: the window is
    // in front, so there is nothing pending to undo it.
    TSharedPtr<ISlateViewport> ViewportInterface = GameViewport->GetViewportInterface().Pin();
    if (!ViewportInterface.IsValid())
    {
        Slate.ActivateGameViewport();
        return;
    }

    const FReply Reply = ViewportInterface->OnFocusReceived(FFocusEvent(EFocusCause::Mouse, static_cast<uint32>(Slate.GetUserIndexForKeyboard())));
    if (!Reply.IsEventHandled())
    {
        Slate.ActivateGameViewport();
        return;
    }

    TArray<TSharedRef<SWindow>> Windows;
    Windows.Add(Window.ToSharedRef());
    FWidgetPath PathToViewport;
    if (FSlateWindowHelper::FindPathToWidget(Windows, GameViewport.ToSharedRef(), PathToViewport, EVisibility::All))
    {
        Slate.ProcessReply(PathToViewport, Reply, nullptr, nullptr, static_cast<uint32>(Slate.GetUserIndexForKeyboard()));
    }
#else
    Slate.ActivateGameViewport();
#endif

    // Only record the recapture on the path that actually took it; every early return above leaves the
    // previous state so the next attempt retries.
    bInputCaptureReleased = false;
}

bool UBlueprintInspectorSubsystem::IsStatementAddress(const UFunction* const Function, const int64 Address)
{
    if (Function == nullptr)
    {
        return false;
    }

    const UClass* const Class = Function->GetOwnerClass();
    if (Class == nullptr)
    {
        return true;
    }

    // Render on demand: the first step into a class runs before the pause session has cached it.
    const int32 Handle = ClassRegistry.EnsureClassHandle(Class);
    const TOptional<FBlueprintRenderedSource> Rendered = ClassRegistry.EnsureClassSource(Handle);
    if (!Rendered.IsSet())
    {
        return true;
    }

    return Rendered->Map.FindLine(Function, Address) != 0;
}

void UBlueprintInspectorSubsystem::SetExceptionFilters(const FExceptionFilterSet& Filters)
{
    ExceptionFilterMask.Store(Filters.ToMask());
}

FExceptionFilterSet UBlueprintInspectorSubsystem::GetExceptionFilters() const
{
    return FExceptionFilterSet::FromMask(ExceptionFilterMask.Load());
}

void UBlueprintInspectorSubsystem::OnScriptException(const UObject* const Context, const FFrame& InFrame, const FBlueprintExceptionInfo& Info)
{
    const EBlueprintExceptionFilter Filter = GetExceptionFilter(Info.GetType());
    if (Filter == EBlueprintExceptionFilter::None)
    {
        // The engine's own debugger channel, not a runtime error.
        return;
    }

    // A stop parks the game thread and hands the viewport's input back, so a worker thread (thread-safe
    // animation) is left to the engine's reporting. The guard covers Blueprint code the debugger itself
    // runs while paused, which must not re-enter a stop.
    if (!IsInGameThread() || FBlueprintInspectorDetours::IsEnabled() || InFrame.Node == nullptr)
    {
        return;
    }

    const FString Text = Info.GetDescription().ToString();
    if (!GetExceptionFilters().Contains(Filter))
    {
        // While a handler is bound the engine skips its own stack trace (DO_BLUEPRINT_GUARD), so a
        // filtered-out error logs it here: attaching must not change what the log shows.
        UE_LOG(LogScript, Warning, TEXT("%s%s"), *Text, *InFrame.GetStackTrace());
        return;
    }

    const UFunction* const Function = InFrame.Node;
    int64 Address = INDEX_NONE;
    TryGetScriptOffset(InFrame, Address);

    // The opcode handler that raised has already consumed its operands, so the exact address can sit
    // past the statement that failed: report the nearest statement at or before it, as a suspended
    // caller's frame does.
    const int32 Handle = ClassRegistry.EnsureClassHandle(Function->GetOwnerClass());
    const int32 Line = Handle != 0 ? Pause.FindLineAtOrBefore(Handle, Function, Address) : 0;

    StepState.Arm(EDebuggingMode::ContinueExecution, GetFrameDepth(InFrame), Function, Address);

    StopAtFrame(InFrame, Address, /*bBreakpointHit=*/false, Line, TEXT("exception"), Text);
}

FBlueprintAuditResult UBlueprintInspectorSubsystem::RunAudit(const int32 MaxFindings)
{
    TArray<UObject*> Classes = ClassRegistry.GatherBlueprintClasses();
    return BlueprintAudit::Run(Classes, MaxFindings);
}

void UBlueprintInspectorSubsystem::OnScriptStep(const UObject* const InObject, const FFrame& InFrame)
{
#if PLATFORM_WINDOWS
    // Re-entry guard: ignore Blueprint code caused by the debugger itself.
    if (FBlueprintInspectorDetours::IsEnabled())
    {
        return;
    }

    if (!InFrame.Node)
    {
        return;
    }

    // Counted before the breakpoint/step early-outs so a capture sees the whole run, not just stops.
    const int64 Address = GetStatementAddress(InFrame);
    HotCapture.CountStep(InFrame.Node, Address);

    // Pauses, variable writes and input capture all assume the game thread. Thread-safe animation
    // Blueprints dispatch on a worker thread; count them but do not stop there.
    if (!IsInGameThread())
    {
        return;
    }

    if (BreakpointStore.NumRecords() == 0 && StepState.IsContinuing())
    {
        return;
    }

    const FBlueprintStepState::FSnapshot Step = StepState.Observe();

    if (BreakpointStore.NumRecords() == 0 && Step.Mode == EDebuggingMode::ContinueExecution)
    {
        return;
    }

    bool bBreakpointHit = BreakpointStore.IsSet(InFrame.Node, Address);
    if (bBreakpointHit)
    {
        FBreakpointKey Key;
        FBreakpointRecord* const Record = BreakpointStore.FindRecord(InFrame.Node, Address, Key);
        if (Record != nullptr)
        {
            // Every dispatch counts, even when a hit condition or a condition suppresses the stop.
            ++Record->HitCount;

            const int64 HitCount = Record->HitCount;
            const FString HitCondition = Record->HitCondition;
            const FString Condition = Record->Condition;
            const FString LogMessage = Record->LogMessage;

            bool bHitCondition = true;
            if (!HitCondition.IsEmpty() && !BlueprintExpression::EvaluateHitCondition(HitCondition, HitCount, bHitCondition))
            {
                UE_LOG(LogBlueprintInspector, Warning, TEXT("Invalid hit condition '%s' on %s.%s@%lld"), *HitCondition, *Key.ClassPath.ToString(), *Key.FunctionName.ToString(), Key.Address);
                bHitCondition = false;
            }

            bool bCondition = true;
            FString ConditionError;
            const FBlueprintExpressionContext ExpressionContext{InObject, InFrame.Node, InFrame.Locals};
            if (bHitCondition && !Condition.IsEmpty() && !BlueprintExpression::EvaluateCondition(ExpressionContext, Condition, bCondition, ConditionError))
            {
                UE_LOG(LogBlueprintInspector, Warning, TEXT("Invalid condition '%s' on %s.%s@%lld: %s"), *Condition, *Key.ClassPath.ToString(), *Key.FunctionName.ToString(), Key.Address, *ConditionError);
                bCondition = false;
            }

            bBreakpointHit = bHitCondition && bCondition;

            // A logpoint is a breakpoint with a message: report and keep running.
            if (bBreakpointHit && !LogMessage.IsEmpty())
            {
                const FString Message = BlueprintExpression::FormatLogMessage(ExpressionContext, LogMessage);
                if (BlueprintInspectorDap)
                {
                    BlueprintInspectorDap->SendOutputEvent(Message + TEXT("\n"));
                }
                bBreakpointHit = false;
            }
        }
    }

    if (IsBreakpointAtStepStart(Step.Mode, bBreakpointHit, InFrame.Node == Step.StartFunction.Get(), Address == Step.StartAddress))
    {
        bBreakpointHit = false;
    }

    if (!bBreakpointHit)
    {
        if (Step.Mode == EDebuggingMode::ContinueExecution)
        {
            return;
        }

        const int32 Depth = GetFrameDepth(InFrame);
        if (!ShouldStopForStep(Step.Mode, Depth, Step.StartDepth, IsStatementAddress(InFrame.Node, Address)))
        {
            return;
        }
    }

    // StopAtFrame sets the re-entry guard before it publishes, so it is not set here as well.
    StepState.Arm(EDebuggingMode::ContinueExecution, GetFrameDepth(InFrame), InFrame.Node, Address);

    StopAtFrame(InFrame, Address, bBreakpointHit, 0);
#endif
}

void UBlueprintInspectorSubsystem::OnScriptJump(const UObject* const InObject, const FFrame& InFrame, const int64 JumpAddress)
{
#if PLATFORM_WINDOWS
    if (FBlueprintInspectorDetours::IsEnabled())
    {
        return;
    }

    if (InFrame.Node == nullptr)
    {
        return;
    }

    // The handler has run, so Code is on the arm it took. A taken arm lands on its operand; a fallthrough
    // lands after the operand. Matching both the jump opcode and the landing is what tells break from
    // continue, and what keeps a breakpoint on one arm from firing on every condition evaluation.
    //
    // Code is normally inside the script, but a partially initialised or native frame can hand a null or
    // foreign pointer.
    const UPTRINT ScriptStart = reinterpret_cast<UPTRINT>(InFrame.Node->Script.GetData());
    const UPTRINT ScriptEnd = ScriptStart + static_cast<UPTRINT>(InFrame.Node->Script.Num());
    const UPTRINT Code = reinterpret_cast<UPTRINT>(InFrame.Code);
    if (InFrame.Code == nullptr || Code < ScriptStart || Code > ScriptEnd)
    {
        return;
    }
    const int64 Landing = static_cast<int64>(Code - ScriptStart);
    HotCapture.CountTransfer(InFrame.Node, JumpAddress, Landing);

    if (!IsInGameThread())
    {
        return;
    }

    if (BreakpointStore.NumTransfers() == 0)
    {
        return;
    }

    if (!BreakpointStore.ContainsTransfer(InFrame.Node, JumpAddress, Landing))
    {
        return;
    }

    // The landed address belongs to the loop header or the loop exit, so it maps to that statement's line.
    // The stop reports the break/continue line the user armed instead.
    const int32 ArmedLine = FindTransferLine(InFrame.Node, JumpAddress, Landing);

    StepState.Arm(EDebuggingMode::ContinueExecution, GetFrameDepth(InFrame), InFrame.Node, JumpAddress);

    StopAtFrame(InFrame, JumpAddress, true, ArmedLine);
#endif
}

void UBlueprintInspectorSubsystem::StopAtFrame(const FFrame& InFrame, const int64 Address, const bool bBreakpointHit, const int32 LineOverride, const FString& Reason, const FString& Text)
{
#if PLATFORM_WINDOWS
    // Set before anything is published: Blueprint code the debugger itself runs must not re-enter a stop.
    FBlueprintInspectorDetours::SetEnabled(true);

    const FString StopReason = !Reason.IsEmpty() ? Reason : FString(bBreakpointHit ? TEXT("breakpoint") : TEXT("step"));
    const FString StopDescription = StopReason == TEXT("exception") ? FString(TEXT("Blueprint runtime error")) : FString();

    UE_LOG(LogBlueprintInspector, Log, TEXT("Stopped at %s+%lld (%s)"), *InFrame.Node->GetName(), Address, *StopReason);

    const TSharedPtr<FEvent, ESPMode::ThreadSafe> Event = Pause.GetEvent();

    const bool bCanPause = Event.IsValid() && BlueprintInspectorDap && BlueprintInspectorDap->HasSession();
    if (bCanPause)
    {
        Event->Reset();
        // Publish the flag before the frame so a request that sees the pause is refused or served from the
        // pre-published state instead of marshalling to a game thread that is already parked. The stopped
        // event is sent only after the frame is published, so a client that waits for it cannot read the gap.
        Pause.PublishPaused();
        Pause.PublishFrame(InFrame, Address, LineOverride);
    }

    if (BlueprintInspectorDap)
    {
        BlueprintInspectorDap->SendStoppedEvent(StopReason, 1, Text, StopDescription);
    }

    if (bCanPause)
    {
        // Give the game viewport's mouse back to the desktop for the duration of the stop, so the
        // cursor can move out to another window without alt-tabbing. The game thread still parks
        // below, so the editor itself stays frozen; this is not an intra-frame debug loop.
        SetInputCaptureReleased(true);

        // Wait in bounded slices. Nothing on the game thread can signal this event if the client
        // disconnects or never resumes, so the editor must be able to fall out on its own.
        bool bForceResume = false;
        while (!Event->Wait(FTimespan::FromMilliseconds(ParkedWaitSliceMilliseconds)))
        {
            Pause.ProcessPendingWrites();

            if (!BlueprintInspectorDap || !BlueprintInspectorDap->HasSession())
            {
                UE_LOG(LogBlueprintInspector, Warning, TEXT("DAP client went away while paused; resuming execution"));
                bForceResume = true;
                break;
            }

            if (IsEngineExitRequested())
            {
                UE_LOG(LogBlueprintInspector, Warning, TEXT("Engine exit requested while paused; resuming execution"));
                bForceResume = true;
                break;
            }
        }

        // A step leaves the pointer yielded: execution stops again at the next statement, and taking
        // the capture back here would clip the cursor only to release it again. A continue, a lost
        // client or an engine exit ends the debug session, so the viewport gets its capture back.
        const EDebuggingMode ResumeMode = StepState.Observe().Mode;

        // The resume can race a disconnect: a step and a lost client may both arrive before the wait
        // ends. With no session left, the stop is over for good, so recapture regardless of the mode.
        if (!BlueprintInspectorDap || !BlueprintInspectorDap->HasSession() || IsEngineExitRequested())
        {
            bForceResume = true;
        }

        if (bForceResume || ResumeMode == EDebuggingMode::ContinueExecution)
        {
            SetInputCaptureReleased(false);
        }

        // The game thread is about to run again: end the pause as one transition, so a write that arrives
        // now is rejected rather than queued for a loop that has stopped draining, and apply the
        // breakpoint requests that were queued while it was parked, before the VM runs another opcode.
        Pause.EndPause();
        ApplyPendingBreakpointRequests();
        Pause.ClearFrame();
    }

    FBlueprintInspectorDetours::SetEnabled(false);
#endif
}

int32 UBlueprintInspectorSubsystem::FindTransferLine(const UFunction* const Function, const int64 JumpAddress, const int64 Landing)
{
    const UClass* const Class = Function != nullptr ? Function->GetOwnerClass() : nullptr;
    if (Class == nullptr)
    {
        return 0;
    }

    const TOptional<FBlueprintRenderedSource> Rendered = ClassRegistry.EnsureClassSource(ClassRegistry.EnsureClassHandle(Class));
    if (!Rendered.IsSet())
    {
        return 0;
    }

    for (int32 Index = 0; Index < Rendered->Map.Lines.Num(); ++Index)
    {
        const FBlueprintLineInfo& Info = Rendered->Map.Lines[Index];
        if (Info.Kind != EBlueprintLineKind::LoopTransfer || Info.ScriptIndex != JumpAddress || Info.TransferTarget != Landing)
        {
            continue;
        }

        // PIE runs a duplicate whose functions are different objects, so the name pair decides here exactly
        // as it does for the breakpoint keys.
        const UFunction* const Owner = Info.OwnerFunction.Get();
        if (Owner == Function || (Owner != nullptr && Owner->GetFName() == Function->GetFName() && GetFunctionClassPath(Owner) == GetFunctionClassPath(Function)))
        {
            return Index + 1;
        }
    }

    return 0;
}
