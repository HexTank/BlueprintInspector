#pragma once

#include "CoreMinimal.h"
#include "BlueprintInspectionTypes.h"
#include "HAL/CriticalSection.h"
#include "Misc/ScopeLock.h"
#include "dap/network.h"
#include "dap/protocol.h"
#include "dap/session.h"
#include <atomic>
#include <memory>
#include <type_traits>

struct FBlueprintAuditResult;
struct FBlueprintSourceMap;

namespace dap
{

// Custom requests for the hot-path capture. cppdap keys handlers by the request type's serialized
// name, so each request's name below is the DAP command string the extension sends.
struct BlueprintHotLine
{
    string className;
    string functionName;
    integer line = 0;
    integer hits = 0;
    // "statement" or "transfer" (a break/continue arm).
    string kind;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintHotLine);

struct BlueprintCaptureState : public Response
{
    boolean capturing = false;
    integer generation = 0;
    integer totalHits = 0;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintCaptureState);

// What produced the run a capture describes. Recorded on the game thread when the capture starts, so a
// request thread never has to walk the world, and carried with the lines so a saved snapshot can say
// which map and build it came from.
struct BlueprintCaptureMetadata
{
    string map;
    string build;
    string engine;
    string project;
    // UTC ISO-8601, read when the capture started.
    string capturedAt;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintCaptureMetadata);

struct BlueprintHotLinesResponse : public Response
{
    BlueprintCaptureState state;
    BlueprintCaptureMetadata metadata;
    array<BlueprintHotLine> lines;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintHotLinesResponse);

struct BlueprintStartCaptureRequest : public Request
{
    using Response = BlueprintCaptureState;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintStartCaptureRequest);

struct BlueprintStopCaptureRequest : public Request
{
    using Response = BlueprintCaptureState;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintStopCaptureRequest);

struct BlueprintResetCaptureRequest : public Request
{
    using Response = BlueprintCaptureState;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintResetCaptureRequest);

struct BlueprintCaptureStateRequest : public Request
{
    using Response = BlueprintCaptureState;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintCaptureStateRequest);

constexpr int64 DefaultHotLinesRequest = 200;
constexpr int64 DefaultCoverageLinesRequest = 50000;
constexpr int64 DefaultAuditFindingsRequest = 500;

struct BlueprintHotLinesRequest : public Request
{
    using Response = BlueprintHotLinesResponse;
    integer maxLines = DefaultHotLinesRequest;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintHotLinesRequest);

struct BlueprintCoverageLine
{
    string className;
    string functionName;
    integer line = 0;
    integer hits = 0;
    // "statement" or "transfer" (a break/continue arm).
    string kind;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintCoverageLine);

struct BlueprintCoverageResponse : public Response
{
    BlueprintCaptureState state;
    array<BlueprintCoverageLine> lines;
    integer renderedClasses = 0;
    integer totalLines = 0;
    integer coveredLines = 0;
    integer uncoveredLines = 0;
    boolean truncated = false;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintCoverageResponse);

struct BlueprintCoverageRequest : public Request
{
    using Response = BlueprintCoverageResponse;
    integer maxLines = DefaultCoverageLinesRequest;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintCoverageRequest);

struct BlueprintAuditFinding
{
    string rule;
    string severity;
    string category;
    string className;
    string functionName;
    integer line = 0;
    string message;
    string evidence;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintAuditFinding);

struct BlueprintAuditResponse : public Response
{
    array<BlueprintAuditFinding> findings;
    integer classes = 0;
    integer functions = 0;
    boolean truncated = false;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintAuditResponse);

struct BlueprintAuditRequest : public Request
{
    using Response = BlueprintAuditResponse;
    integer maxFindings = DefaultAuditFindingsRequest;
};
DAP_DECLARE_STRUCT_TYPEINFO(BlueprintAuditRequest);

} // namespace dap

// A callback result that says whether the game thread actually serviced the work. A timeout is not the
// same as a legitimate empty answer, so the DAP layer can report it instead of returning blanks.
template <typename TResult>
struct TGameThreadCallResult
{
    TResult Value{};
    // True by default: a default-constructed result, including the one MakeGatedCallback returns when
    // the callback gate is closed, must read as "the game thread did not service this" rather than as a
    // legitimate empty answer. A handler that really returned data constructs {Value, false}.
    bool bTimedOut = true;
    // True when the game thread is parked: the work was refused, not abandoned. The handler can tell the
    // client to retry after the VM resumes instead of returning an empty answer.
    bool bUnavailable = false;
};

using FAcquireClassListDelegate = TFunction<TGameThreadCallResult<TArray<FFunctionProperties>>()>;
using FAcquireClassFunctions = TFunction<TGameThreadCallResult<FString>(int32)>;

using FAcquireSourceMap = TFunction<TGameThreadCallResult<FBlueprintSourceMap>(const FBlueprintSourceIdentity&)>;
// Applies DAP's replace-the-whole-set semantics and reports one resolution per requested line, in order.
using FApplyBreakpoints = TFunction<TGameThreadCallResult<TArray<FBlueprintBreakpointResolution>>(const FBlueprintSourceIdentity&, const TArray<FBlueprintBreakpointRequest>&)>;
// False when the VM pause hooks are not installed, so breakpoints cannot hit. Unset means the caller
// has no pause path to report on.
using FIsPauseAvailable = TFunction<bool()>;
using FAcquireStackFrames = TFunction<TArray<FBlueprintStackFrame>(int32)>;
using FAcquireScopes = TFunction<TArray<FBlueprintScope>(int32)>;
using FAcquireVariables = TFunction<TArray<FBlueprintVariable>(int32)>;
using FSetVariable = TFunction<FBlueprintSetVariableResult(int32, const FString&, const FString&)>;

using FEvaluateExpression = TFunction<FBlueprintEvaluateResult(int32 FrameId, const FString& Expression)>;
using FContinueExecution = TFunction<void()>;

// Returns false when there is no paused frame to step from.
using FStepExecution = TFunction<bool(EBlueprintStepKind)>;
// Capture controls return the state they left behind.
using FCaptureControl = TFunction<FBlueprintHotCaptureStatus()>;
using FAcquireHotLines = TFunction<TGameThreadCallResult<FBlueprintHotLinesResult>(int32 MaxLines)>;
using FAcquireCaptureMetadata = TFunction<TGameThreadCallResult<FBlueprintCaptureMetadata>()>;
using FRunAudit = TFunction<TGameThreadCallResult<FBlueprintAuditResult>(int32 MaxFindings)>;
// The client's exception filters, as the ids it sent. The subsystem owns what the ids mean.
using FApplyExceptionFilters = TFunction<void(const TArray<FString>& Filters)>;

using FAcquireCoverage = TFunction<TGameThreadCallResult<FBlueprintCoverageResult>(int32 MaxLines)>;

// The subsystem installs this set once per session and clears it before teardown. Handlers copy the
// function they need under a mutex, so the bundle cannot be assigned mid-copy. That copy does not
// keep the callee's captured state alive: a handler that was copied just before ClearCallbacks() may
// still run afterwards (the cppdap receive thread is joined only when the session is destroyed).
// GetCallback therefore hands out a copy that checks FBlueprintInspectorCallbackGate, and every
// callback still captures a weak reference (TWeakObjectPtr) and re-validates it before use.
struct FBlueprintInspectorDapCallbacks
{
    FAcquireClassListDelegate AcquireClassList;
    FAcquireClassFunctions AcquireClassFunctions;
    FAcquireSourceMap AcquireSourceMap;
    FApplyBreakpoints ApplyBreakpoints;
    FIsPauseAvailable IsPauseAvailable;
    FAcquireStackFrames AcquireStackFrames;
    FAcquireScopes AcquireScopes;
    FAcquireVariables AcquireVariables;
    FSetVariable SetVariable;
    FEvaluateExpression EvaluateExpression;
    FContinueExecution ContinueExecution;
    FStepExecution StepExecution;
    FCaptureControl StartCapture;
    FCaptureControl StopCapture;
    FCaptureControl ResetCapture;
    FCaptureControl QueryCapture;
    FCaptureControl ClearCapture;
    FAcquireHotLines AcquireHotLines;
    FAcquireCaptureMetadata AcquireCaptureMetadata;
    FAcquireCoverage AcquireCoverage;
    FRunAudit RunAudit;
    FApplyExceptionFilters ApplyExceptionFilters;
};

class FSessionAuth
{
public:
    enum class EQueueResult : uint8
    {
        Allowed,         // this generation is authenticated; run the work now
        Queued,          // queued behind an existing deadline
        QueuedWithTimer, // queued and armed the deadline for this generation
        Rejected         // a stale generation, or the pending queue is full
    };

    enum class EHandshakeResult : uint8
    {
        Accepted,
        Rejected,
        StaleGeneration // the handshake came from a connection that no longer owns the state
    };

    // A local client can send gated requests faster than it authenticates. The queue holds copied
    // payloads and unanswered Respond callbacks, so it needs a bound.
    static constexpr int32 MaxPendingRequests = 256;

    FString Begin(uint64& OutGeneration)
    {
        TArray<TFunction<void(bool)>> Rejected;
        FString NewToken;
        {
            FScopeLock Guard(&Lock);
            Rejected = MoveTemp(Pending);
            Pending.Reset();
            bAuthenticated = false;
            bTimeoutStarted = false;
            Attempts = 0;
            ++Generation;
            OutGeneration = Generation;
            Token = FGuid::NewGuid().ToString(EGuidFormats::Digits).ToLower();
            NewToken = Token;
        }

        // A new connection must answer the previous generation's queued requests, not drop them: each
        // entry owns a cppdap Respond callback.
        RejectQueued(Rejected);
        return NewToken;
    }

    // An authenticated connection stays authenticated: a duplicate or stale handshake must not demote it.
    // The handshake must still present something, so an empty token cannot ride the sticky flag.
    EHandshakeResult Authenticate(const FString& Provided, const uint64 InGeneration, int32& OutAttempts, uint64& OutGeneration)
    {
        FScopeLock Guard(&Lock);
        OutGeneration = Generation;
        if (Generation != InGeneration)
        {
            OutAttempts = Attempts;
            return EHandshakeResult::StaleGeneration;
        }
        if (!Token.IsEmpty() && TokensEqual(Provided, Token))
        {
            bAuthenticated = true;
            Attempts = 0;
            OutAttempts = 0;
            return EHandshakeResult::Accepted;
        }
        OutAttempts = ++Attempts;
        return EHandshakeResult::Rejected;
    }

    [[nodiscard]] bool IsAuthenticated() const
    {
        FScopeLock Guard(&Lock);
        return bAuthenticated;
    }

    [[nodiscard]] bool IsAuthenticatedFor(const uint64 InGeneration) const
    {
        FScopeLock Guard(&Lock);
        return bAuthenticated && Generation == InGeneration;
    }

    // The generation is the connection the request was registered on. A request from an older
    // connection is rejected rather than left in the queue where a newer connection's handshake would
    // release it.
    EQueueResult Queue(const uint64 InGeneration, const TFunction<void(bool)>& Work, uint64& OutGeneration, uint64& OutBatch)
    {
        FScopeLock Guard(&Lock);
        OutGeneration = Generation;
        OutBatch = 0;
        if (Generation != InGeneration)
        {
            return EQueueResult::Rejected;
        }
        if (bAuthenticated)
        {
            return EQueueResult::Allowed;
        }
        if (Pending.Num() >= MaxPendingRequests)
        {
            return EQueueResult::Rejected;
        }
        Pending.Add(Work);
        if (bTimeoutStarted)
        {
            return EQueueResult::Queued;
        }
        bTimeoutStarted = true;
        OutGeneration = Generation;
        OutBatch = ++TimeoutBatch;
        return EQueueResult::QueuedWithTimer;
    }

    TArray<TFunction<void(bool)>> Drain()
    {
        FScopeLock Guard(&Lock);
        TArray<TFunction<void(bool)>> Out = MoveTemp(Pending);
        Pending.Reset();
        bTimeoutStarted = false;
        return Out;
    }

    // The expiry decision and the drain happen under one lock, so a client that authenticates between
    // them cannot have its queued requests rejected.
    bool DrainIfExpired(const uint64 InGeneration, const uint64 InBatch, TArray<TFunction<void(bool)>>& Out)
    {
        FScopeLock Guard(&Lock);
        const bool bExpired = Generation == InGeneration && !bAuthenticated && bTimeoutStarted && TimeoutBatch == InBatch;
        if (!bExpired)
        {
            return false;
        }
        Out = MoveTemp(Pending);
        Pending.Reset();
        bTimeoutStarted = false;
        return true;
    }

    [[nodiscard]] bool IsUnauthenticatedFor(const uint64 InGeneration) const
    {
        FScopeLock Guard(&Lock);
        return Generation == InGeneration && !bAuthenticated;
    }

    // True only for the deadline Queue armed and that no drain has since retired: a timer left over from
    // an earlier batch must not reject requests queued with a newer deadline.
    [[nodiscard]] bool IsExpiredFor(const uint64 InGeneration, const uint64 InBatch) const
    {
        FScopeLock Guard(&Lock);
        return Generation == InGeneration && !bAuthenticated && bTimeoutStarted && TimeoutBatch == InBatch;
    }

    // Clears this generation. Returns the token it owned, or an empty string when a newer generation owns
    // the state (so the caller cannot delete a newer connection's token file).
    FString Reset(const uint64 InGeneration)
    {
        TArray<TFunction<void(bool)>> Rejected;
        FString Cleared;
        {
            FScopeLock Guard(&Lock);
            if (Generation != InGeneration)
            {
                return FString();
            }
            Rejected = MoveTemp(Pending);
            Pending.Reset();
            bAuthenticated = false;
            bTimeoutStarted = false;
            ++TimeoutBatch;
            Attempts = 0;
            Cleared = Token;
            Token.Reset();
            ++Generation;
        }

        // The queued work owns the cppdap Respond callback for this generation; reject it rather than
        // leaving those requests unanswered.
        RejectQueued(Rejected);
        return Cleared;
    }

    // EndSession has no connection generation to name; clear everything.
    void ResetAll()
    {
        TArray<TFunction<void(bool)>> Rejected;
        {
            FScopeLock Guard(&Lock);
            Rejected = MoveTemp(Pending);
            Pending.Reset();
            bAuthenticated = false;
            bTimeoutStarted = false;
            ++TimeoutBatch;
            Attempts = 0;
            Token.Reset();
            ++Generation;
        }

        RejectQueued(Rejected);
    }

    [[nodiscard]] FString GetToken() const
    {
        FScopeLock Guard(&Lock);
        return Token;
    }

private:
    // Runs outside Lock: the queued work owns a cppdap Respond callback, which may take other locks.
    static void RejectQueued(TArray<TFunction<void(bool)>>& InPending)
    {
        for (TFunction<void(bool)>& Work : InPending)
        {
            if (Work)
            {
                Work(false);
            }
        }
        InPending.Reset();
    }

    static bool TokensEqual(const FString& A, const FString& B)
    {
        if (A.Len() != B.Len())
        {
            return false;
        }
        uint32 Difference = 0;
        for (int32 Index = 0; Index < A.Len(); ++Index)
        {
            Difference |= static_cast<uint32>(A[Index]) ^ static_cast<uint32>(B[Index]);
        }
        return Difference == 0;
    }

    mutable FCriticalSection Lock;
    bool bAuthenticated = false;
    bool bTimeoutStarted = false;
    int32 Attempts = 0;
    uint64 Generation = 0;
    uint64 TimeoutBatch = 0;
    FString Token;
    TArray<TFunction<void(bool)>> Pending;
};

// The single client-accept slot: the generation it owns and whether its socket closed before the
// session was published. Caller holds SessionMutex.
class FConnectingSlot
{
public:
    [[nodiscard]] bool IsConnecting() const { return bConnecting; }
    [[nodiscard]] bool IsFor(const uint64 InGeneration) const { return bConnecting && Generation == InGeneration; }

    void Begin()
    {
        bConnecting = true;
        Closed = false;
        Generation = 0;
    }

    void Abort() { bConnecting = false; }
    void SetGeneration(const uint64 InGeneration)
    {
        check(bConnecting);
        check(InGeneration != 0);
        Generation = InGeneration;
    }

    void MarkClosed(const uint64 InGeneration)
    {
        check(bConnecting);
        check(Generation != 0);
        if (Generation == InGeneration)
        {
            Closed = true;
        }
    }

    bool Finish()
    {
        const bool bWasClosed = Closed;
        bConnecting = false;
        Generation = 0;
        Closed = false;
        return bWasClosed;
    }

private:
    bool bConnecting = false;
    uint64 Generation = 0;
    bool Closed = false;
};

// The published client connection: the session, its socket and the accept generation, published and
// retired as one unit. Caller holds SessionMutex. Keeping the three together is what stops a close or a
// disconnect decision taken for one generation from reaching the connection that replaced it.
class FPublishedConnection
{
public:
    [[nodiscard]] bool IsPublished() const { return Session.IsValid(); }
    [[nodiscard]] uint64 GetGeneration() const { return Generation; }
    [[nodiscard]] TSharedPtr<dap::Session> GetSession() const { return Session; }

    void Publish(const TSharedPtr<dap::Session>& InSession, const std::shared_ptr<dap::ReaderWriter>& InSocket, const uint64 InGeneration)
    {
        Session = InSession;
        Socket = InSocket;
        Generation = InGeneration;
    }

    // The socket a close decision for InGeneration may reach; null when another connection owns the
    // slot, so a stale timeout or attempt limit cannot close the newer socket.
    [[nodiscard]] std::shared_ptr<dap::ReaderWriter> SocketForClose(const uint64 InGeneration) const
    {
        return IsPublished() && Generation == InGeneration ? Socket : nullptr;
    }

    // Retires the connection when InGeneration owns it. Returns the session so the caller can destroy it
    // outside the lock; null when the published connection belongs to another generation.
    [[nodiscard]] TSharedPtr<dap::Session> Retire(const uint64 InGeneration)
    {
        return IsPublished() && Generation == InGeneration ? TakeSession() : nullptr;
    }

    // Retires whichever connection is published (host teardown).
    [[nodiscard]] TSharedPtr<dap::Session> RetireAll()
    {
        return TakeSession();
    }

private:
    TSharedPtr<dap::Session> TakeSession()
    {
        TSharedPtr<dap::Session> Retired = MoveTemp(Session);
        Socket.reset();
        Generation = 0;
        return Retired;
    }

    TSharedPtr<dap::Session> Session;
    std::shared_ptr<dap::ReaderWriter> Socket;
    uint64 Generation = 0;
};

// A handler can copy a callback just before ClearCallbacks() and invoke it afterwards, because the cppdap
// receive thread is joined only when the session is destroyed. Closing this gate turns such a copy into a
// no-op, so the lifetime rule is enforced by GetCallback instead of by every registration.
struct FBlueprintInspectorCallbackGate
{
    std::atomic<bool> bOpen{true};
};

// Wraps an installed callback so a copy that outlives ClearCallbacks() observes the closed gate.
template <typename TResult, typename... TArgs>
TFunction<TResult(TArgs...)> MakeGatedCallback(TFunction<TResult(TArgs...)> Callback, const TSharedPtr<FBlueprintInspectorCallbackGate, ESPMode::ThreadSafe>& Gate)
{
    if (!Callback)
    {
        return TFunction<TResult(TArgs...)>();
    }

    return [Callback = MoveTemp(Callback), Gate](TArgs... Args) -> TResult {
        if (!Gate.IsValid() || !Gate->bOpen.load())
        {
            if constexpr (!std::is_void_v<TResult>)
            {
                return TResult();
            }
            else
            {
                return;
            }
        }
        return Callback(Forward<TArgs>(Args)...);
    };
}

class FUBlueprintInspectorDap : public TSharedFromThis<FUBlueprintInspectorDap>
{
public:
    // The lifecycle entry points call AsShared(), which is only valid for an object owned by a
    // TSharedPtr. Construct through Create() (or MakeShared) instead of putting this on the stack.
    static TSharedRef<FUBlueprintInspectorDap> Create();
    ~FUBlueprintInspectorDap();

    // Port the listener binds: 19021 unless -BlueprintInspectorDapPort=<n> overrides it. The automation
    // suite sets it per run so a suite cannot collide with an editor that is already listening.
    static int32 ListeningPort();
    // Absolute paths of the per-connection token. The file name carries the port when it was overridden,
    // so a test run never reads or deletes the token of a default-port session.
    static FString GetTokenFilePath();
    static FString GetSharedTokenFilePath();

private:
    FUBlueprintInspectorDap() = default;

public:
    // Starts the loopback listener. False when the server could not be created or bound, so the caller
    // can distinguish "listening" from "not listening" instead of assuming a session exists.
    [[nodiscard]] bool StartSession();
    void EndSession();
    void SetCallbacks(const FBlueprintInspectorDapCallbacks& InCallbacks);
    void ClearCallbacks();
    // Text is the exception message a client shows with the stop; Description is its long form.
    void SendStoppedEvent(const FString& Reason, int32 ThreadId, const FString& Text = FString(), const FString& Description = FString());
    void SendOutputEvent(const FString& Output);
    [[nodiscard]] bool HasSession() const;

    dap::ResponseOrError<dap::InitializeResponse> InitializeRequest(const dap::InitializeRequest&);
    void InitializeResponse(const dap::ResponseOrError<dap::InitializeResponse>& Response, const TSharedPtr<dap::Session>& Session);
    dap::ResponseOrError<dap::LaunchResponse> LaunchRequest(const dap::LaunchRequest&, uint64 Generation);
    dap::ResponseOrError<dap::ThreadsResponse> ThreadsRequest(const dap::ThreadsRequest&);

    dap::ResponseOrError<dap::AttachResponse> AttachRequest(const dap::AttachRequest&);
    dap::ResponseOrError<dap::LoadedSourcesResponse> LoadedSourcesRequest(const dap::LoadedSourcesRequest&);
    dap::ResponseOrError<dap::BreakpointLocationsResponse> BreakpointLocationsRequest(const dap::BreakpointLocationsRequest&);
    dap::ResponseOrError<dap::StackTraceResponse> StackTraceRequest(const dap::StackTraceRequest&);
    dap::ResponseOrError<dap::ScopesResponse> ScopesRequest(const dap::ScopesRequest&);
    dap::ResponseOrError<dap::VariablesResponse> VariablesRequest(const dap::VariablesRequest&);
    dap::ResponseOrError<dap::SetVariableResponse> SetVariableRequest(const dap::SetVariableRequest&);
    dap::ResponseOrError<dap::EvaluateResponse> EvaluateRequest(const dap::EvaluateRequest&);
    dap::ResponseOrError<dap::PauseResponse> PauseRequest(const dap::PauseRequest&);
    dap::ResponseOrError<dap::ContinueResponse> ContinueRequest(const dap::ContinueRequest&);
    dap::ResponseOrError<dap::NextResponse> NextRequest(const dap::NextRequest&);
    dap::ResponseOrError<dap::StepInResponse> StepInRequest(const dap::StepInRequest&);
    dap::ResponseOrError<dap::StepOutResponse> StepOutRequest(const dap::StepOutRequest&);
    dap::ResponseOrError<dap::SetBreakpointsResponse> SetBreakpointsRequest(const dap::SetBreakpointsRequest&);
    dap::ResponseOrError<dap::SourceResponse> SourceRequest(const dap::SourceRequest&);
    dap::ResponseOrError<dap::DisconnectResponse> DisconnectRequest(const dap::DisconnectRequest&, uint64 Generation);
    dap::ResponseOrError<dap::ConfigurationDoneResponse> ConfigurationDoneRequest(const dap::ConfigurationDoneRequest&);
    dap::ResponseOrError<dap::BlueprintCaptureState> StartCaptureRequest(const dap::BlueprintStartCaptureRequest&);
    dap::ResponseOrError<dap::BlueprintCaptureState> StopCaptureRequest(const dap::BlueprintStopCaptureRequest&);
    dap::ResponseOrError<dap::BlueprintCaptureState> ResetCaptureRequest(const dap::BlueprintResetCaptureRequest&);
    dap::ResponseOrError<dap::BlueprintCaptureState> CaptureStateRequest(const dap::BlueprintCaptureStateRequest&);
    dap::ResponseOrError<dap::BlueprintHotLinesResponse> HotLinesRequest(const dap::BlueprintHotLinesRequest&);
    dap::ResponseOrError<dap::BlueprintCoverageResponse> CoverageRequest(const dap::BlueprintCoverageRequest&);
    dap::ResponseOrError<dap::BlueprintAuditResponse> AuditRequest(const dap::BlueprintAuditRequest&);
    // Standard request: the client selects which runtime errors stop execution.
    dap::ResponseOrError<dap::SetExceptionBreakpointsResponse> SetExceptionBreakpointsRequest(const dap::SetExceptionBreakpointsRequest&);

    // Runs Work now when the connection that registered it has presented the session token, otherwise
    // holds it until that token arrives (or the authentication deadline expires, in which case Work
    // runs with false). A request from a retired connection runs with false instead of waiting for a
    // newer connection's handshake.
    void DispatchAuthenticatedForGeneration(uint64 Generation, const TFunction<void(bool)>& Work);
    [[nodiscard]] bool IsAuthenticated() const;
    // Presents a session token. Public so the launch request and the auth-gate test share one path.
    [[nodiscard]] bool Authenticate(const FString& Provided, uint64 Generation);

private:
    template <typename TCallback>
    TCallback GetCallback(TCallback FBlueprintInspectorDapCallbacks::* Member) const
    {
        TCallback Callback;
        TSharedPtr<FBlueprintInspectorCallbackGate, ESPMode::ThreadSafe> Gate;
        {
            FScopeLock Lock(&CallbackMutex);
            Callback = Callbacks.*Member;
            Gate = CallbackGate;
        }

        return MakeGatedCallback(MoveTemp(Callback), Gate);
    }

    void OnClientConnected(const std::shared_ptr<dap::ReaderWriter>& Socket);
    void OnClientDisconnected(const TWeakPtr<dap::Session>& WeakSession, uint64 Generation);
    TSharedPtr<dap::Session> GetSession() const;

    // The published session together with the accept generation it belongs to. Auth decisions must be
    // made against this pair: IsAuthenticated() and GetSession() taken separately can straddle a
    // reconnect and deliver one connection's data to another.
    struct FSessionSnapshot
    {
        TSharedPtr<dap::Session> Session;
        uint64 Generation = 0;
    };
    [[nodiscard]] FSessionSnapshot GetSessionSnapshot() const;

    void FlushPendingRequests(bool bAllowed);
    // Generation and Batch name the connection and deadline Queue armed; a timer for a connection that
    // has gone away, or for a batch a drain has retired, must not reject the current one.
    void TimeoutPendingRequests(uint64 Generation, uint64 Batch);
    void WriteTokenFile(const FString& Token);
    void DeleteTokenFile();
    void DeleteTokenFileIfMatches(const FString& ExpectedToken);
    // Closes only the socket published for Generation; a stale timer for an earlier connection must not
    // close the one that replaced it.
    void CloseClientSocket(uint64 Generation);
    void CloseIfUnauthenticated(uint64 Generation);

    TUniquePtr<dap::net::Server> Server;
    // The published connection (session, socket, generation); published and retired under SessionMutex.
    FPublishedConnection Published;
    mutable FCriticalSection SessionMutex;
    mutable FCriticalSection CallbackMutex;
    FBlueprintInspectorDapCallbacks Callbacks;
    // Replaced by every SetCallbacks and closed by ClearCallbacks; see FBlueprintInspectorCallbackGate.
    TSharedPtr<FBlueprintInspectorCallbackGate, ESPMode::ThreadSafe> CallbackGate;
    // Lifecycle transitions are serialized by SessionMutex: a start/connect in progress owns the slot
    // even though Server/Session are not published yet, so a second start or a teardown cannot observe
    // a half-built session.
    bool bStarting = false;
    bool bShuttingDown = false;
    FConnectingSlot ConnectingSlot;

    // A token is generated per client connection and written to Saved/BlueprintInspector/dap-token.
    // Requests that can read or mutate debugger state are held until the client presents it; the
    // handshake exists because another local account must not be able to attach. A process running as
    // the same user can read the file, so this is a session gate, not a secret against that user.
    FSessionAuth Auth;

    static constexpr int32 AuthTimeoutSeconds = 5;
    static constexpr int32 MaxAuthAttempts = 5;
    // VS Code sends its debug type as `adapterID` when it attaches through `debugServer`. That value is
    // public in package.json, so it is a protocol sanity check; the session token above is the gate.
    static constexpr const char* DebugType = "blueprint-inspector";
};
