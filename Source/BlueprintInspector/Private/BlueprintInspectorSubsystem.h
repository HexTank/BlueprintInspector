#pragma once

#include "CoreMinimal.h"
#include "BlueprintAudit.h"
#include "BlueprintBreakpointStore.h"
#include "BlueprintBreakpointTypes.h"
#include "BlueprintClassRegistry.h"
#include "BlueprintExceptionFilter.h"
#include "BlueprintHotCapture.h"
#include "BlueprintInspectionTypes.h"
#include "BlueprintInspectorDAP.h"
#include "BlueprintKeys.h"
#include "BlueprintPauseSession.h"
#include "BlueprintPauseState.h"
#include "BlueprintPausedFrame.h"
#include "BlueprintSourceMap.h"
#include "BlueprintStepRules.h"
#include "BlueprintStepState.h"
#include "Subsystems/GameInstanceSubsystem.h"

#include "BlueprintInspectorSubsystem.generated.h"

class UFunction;
class UPackage;
struct FFrame;
struct FBlueprintExceptionInfo;

// Windows only: Initialize installs the detour hooks behind PLATFORM_WINDOWS.
UCLASS(Transient)
class BLUEPRINTINSPECTOR_API UBlueprintInspectorSubsystem : public UGameInstanceSubsystem
{
    GENERATED_BODY()

public:
    void Initialize(FSubsystemCollectionBase& Collection) override;
    void Deinitialize() override;

    // Game thread only: mutates the class registry/source cache and reads the PIE world. Off-thread
    // callers must marshal (the DAP callbacks use MarshalUnlessPaused).
    TArray<FFunctionProperties> AcquireClassList();

#if WITH_DEV_AUTOMATION_TESTS
    // Test hooks for the live DAP integration test. They arm the same breakpoint state the DAP
    // setBreakpoints handler writes, without needing a client request before the game thread parks.
    bool TestEnsureBreakpoint(const UFunction* Function, int64 Address);
    bool TestClearBreakpoint(const UFunction* Function, int64 Address);
    // Runs the pause-aware marshalling path with a cache that may end the pause, then reports whether the
    // game-thread work ran. False when the request was refused as unavailable.
    bool TestMarshalUnlessPaused(bool bResumeDuringCache, int32& OutValue);
    bool IsReferenceOwnerForTest() const;
    static bool HasReferenceOwnerForTest();
    static bool BindScriptHandlersForTest(UBlueprintInspectorSubsystem* Owner);
    static void ClearReferenceOwnerForTest(UBlueprintInspectorSubsystem* Owner);
#endif

    // Read-only view of the pause handshake. The flag and the setVariable queue behind it stay private:
    // only the subsystem ends a pause, and it always does so as one queue transition.
    bool IsPaused() const { return Pause.IsPaused(); }

    // Blueprint runtime-error filters. Written by an authenticated client, read where the VM raises.
    void SetExceptionFilters(const FExceptionFilterSet& Filters);
    FExceptionFilterSet GetExceptionFilters() const;
    // Heuristic pre-ship checks over every Blueprint class. Game thread only.
    FBlueprintAuditResult RunAudit(int32 MaxFindings);

private:
    const UPackage* GetPIEPackage() const;

    void ApplyPendingBreakpointRequests();
    void ReleaseReferenceOwner();

    void OnScriptStep(const UObject* InObject, const FFrame& InFrame);

    // The post-dispatch half of a break/continue stop. OnScriptStep cannot decide it: the jump opcode runs
    // on both arms, and only the handler leaves Code on the arm that was taken.
    void OnScriptJump(const UObject* InObject, const FFrame& InFrame, int64 JumpAddress);

    // Parks the game thread and publishes the stop. LineOverride reports an armed line instead of the one
    // the landed address maps to, which a break/continue stop needs because it lands on the loop header or
    // the loop exit rather than on the line the user armed. 0 means no override; line numbers are 1-based,
    // and FindTransferLine also returns 0 when the class no longer renders the line.
    // Reason/Text override the stopped event: a breakpoint and a step report their own reason, an
    // exception adds the error text the client shows with the stop.
    void StopAtFrame(const FFrame& InFrame, int64 Address, bool bBreakpointHit, int32 LineOverride, const FString& Reason = FString(), const FString& Text = FString());
    // FBlueprintCoreDelegates::OnScriptException: raised on the game thread by the VM's error paths.
    void OnScriptException(const UObject* Context, const FFrame& InFrame, const FBlueprintExceptionInfo& Info);

    // Releases the paused game thread with a step mode. False when nothing is paused or step-out has
    // no caller (the root frame).
    bool RequestStep(EBlueprintStepKind Kind);
    bool IsStatementAddress(const UFunction* Function, int64 Address);

    // Hands the game viewport's mouse capture and cursor lock back to the desktop while the VM is
    // stopped, and takes them back when a resume ends the debug session. The game thread still parks,
    // so the editor itself stays frozen; only the pointer is yielded. The debugger window is expected
    // to take focus at a stop, so a resume only recaptures when the game window is back in front.
    void SetInputCaptureReleased(bool bReleased);

    // The 1-based line of the armed break/continue transfer, or 0 when the class no longer renders it.
    int32 FindTransferLine(const UFunction* Function, int64 JumpAddress, int64 Landing);

    // The classes advertised to a DAP client, and the rendered source for each. Owns the handle lock and
    // the source cache: the game thread issues and binds handles, any thread may read.
    FBlueprintClassRegistry ClassRegistry;

    // The paused VM view: the stopped call chain, its variable groups and their generation. Holds the
    // registry for line lookups and for rendering a class before the game thread parks.
    FBlueprintPauseSession Pause{ClassRegistry};

    // The armed breakpoints: records, break/continue arms and the address index. Holds the registry for
    // line resolution only.
    FBlueprintBreakpointStore BreakpointStore{ClassRegistry};

    // The hot-path counters, their generation and the metadata cache. Holds the registry for report
    // resolution only.
    FBlueprintHotCapture HotCapture{ClassRegistry};

    // Where the next stop is allowed to be: the mode and the frame the step started from. Its own lock;
    // a dispatch takes one snapshot rather than four reads.
    FBlueprintStepState StepState;

    TAtomic<int32> ExceptionFilterMask{0};
    FDelegateHandle ScriptExceptionHandle;

    TSharedPtr<FUBlueprintInspectorDap> BlueprintInspectorDap;

    bool bInputCaptureReleased = false;

    // Process-wide owner election. Only the owner instance has a pause event, a DAP session and bound
    // script detours; every other GameInstance's subsystem is inert (IsPaused() is always false and the
    // debugger state below is unused). Owner shutdown tears the session down for all live instances.
    static FCriticalSection ReferenceOwnerLock;
    static TWeakObjectPtr<UObject> ReferenceOwner;
};