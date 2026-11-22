#include "BlueprintInspectorDAP.h"
#include "BlueprintInspector.h"

#include "BlueprintAudit.h"
#include "BlueprintExceptionFilter.h"
#include "BlueprintSourceMap.h"

#include <exception>
#include <memory>

#include "Async/Async.h"
#include "Containers/StringConv.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"

namespace
{
int32 NarrowDapInt(const int64 Value)
{
    return static_cast<int32>(FMath::Clamp<int64>(Value, 0, TNumericLimits<int32>::Max()));
}
} // namespace

namespace dap
{

// The request type's serialized name is the DAP command string (cppdap keys handlers by it).
DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintHotLine, "BlueprintHotLine", DAP_FIELD(className, "className"), DAP_FIELD(functionName, "functionName"), DAP_FIELD(line, "line"), DAP_FIELD(hits, "hits"), DAP_FIELD(kind, "kind"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintCaptureState, "BlueprintCaptureState", DAP_FIELD(capturing, "capturing"), DAP_FIELD(generation, "generation"), DAP_FIELD(totalHits, "totalHits"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintCaptureMetadata, "BlueprintCaptureMetadata", DAP_FIELD(map, "map"), DAP_FIELD(build, "build"), DAP_FIELD(engine, "engine"), DAP_FIELD(project, "project"), DAP_FIELD(capturedAt, "capturedAt"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintHotLinesResponse, "BlueprintHotLinesResponse", DAP_FIELD(state, "state"), DAP_FIELD(metadata, "metadata"), DAP_FIELD(lines, "lines"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintStartCaptureRequest, "blueprintInspector.startCapture");
DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintStopCaptureRequest, "blueprintInspector.stopCapture");
DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintResetCaptureRequest, "blueprintInspector.resetCapture");
DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintCaptureStateRequest, "blueprintInspector.captureState");
DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintHotLinesRequest, "blueprintInspector.hotLines", DAP_FIELD(maxLines, "maxLines"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintCoverageLine, "BlueprintCoverageLine", DAP_FIELD(className, "className"), DAP_FIELD(functionName, "functionName"), DAP_FIELD(line, "line"), DAP_FIELD(hits, "hits"), DAP_FIELD(kind, "kind"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintCoverageResponse, "BlueprintCoverageResponse", DAP_FIELD(state, "state"), DAP_FIELD(lines, "lines"), DAP_FIELD(renderedClasses, "renderedClasses"), DAP_FIELD(totalLines, "totalLines"), DAP_FIELD(coveredLines, "coveredLines"), DAP_FIELD(uncoveredLines, "uncoveredLines"), DAP_FIELD(truncated, "truncated"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintCoverageRequest, "blueprintInspector.coverage", DAP_FIELD(maxLines, "maxLines"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintAuditFinding, "BlueprintAuditFinding", DAP_FIELD(rule, "rule"), DAP_FIELD(severity, "severity"), DAP_FIELD(category, "category"), DAP_FIELD(className, "className"), DAP_FIELD(functionName, "functionName"), DAP_FIELD(line, "line"), DAP_FIELD(message, "message"), DAP_FIELD(evidence, "evidence"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintAuditResponse, "BlueprintAuditResponse", DAP_FIELD(findings, "findings"), DAP_FIELD(classes, "classes"), DAP_FIELD(functions, "functions"), DAP_FIELD(truncated, "truncated"));

DAP_IMPLEMENT_STRUCT_TYPEINFO(BlueprintAuditRequest, "blueprintInspector.audit", DAP_FIELD(maxFindings, "maxFindings"));

} // namespace dap

namespace
{
dap::BlueprintCaptureState ToCaptureState(const FBlueprintHotCaptureStatus& Status)
{
    dap::BlueprintCaptureState Result;
    Result.capturing = Status.bCapturing;
    Result.generation = Status.Generation;
    Result.totalHits = Status.TotalHits;
    return Result;
}

FString DescribeSource(const dap::Source& Source)
{
    return FString::Printf(TEXT("sourceReference=%lld path='%hs' name='%hs'"), static_cast<int64>(Source.sourceReference.value(0)), Source.path.has_value() ? Source.path.value().c_str() : "", Source.name.has_value() ? Source.name.value().c_str() : "");
}

FBlueprintSourceIdentity DescribeIdentity(const dap::Source& Source)
{
    static const TCHAR* const PathPrefix = TEXT("blueprint-source:Blueprints/");
    static const TCHAR* const PathSuffix = TEXT(".cpp");

    FBlueprintSourceIdentity Identity;
    const int64 RequestedReference = Source.sourceReference.value(0);
    Identity.SourceReference = RequestedReference >= 0 && RequestedReference <= MAX_int32 ? static_cast<int32>(RequestedReference) : 0;

    if (Source.path.has_value())
    {
        // The extension opens documents as `blueprint-source:Blueprints/<Class>.cpp?name=<Class>` so the
        // provider can find the class. The query is editor bookkeeping, not part of the identity, and a
        // client may also encode it into the path; drop it before matching.
        FString Path = UTF8_TO_TCHAR(Source.path.value().c_str());
        int32 QueryIndex = INDEX_NONE;
        if (Path.FindChar(TEXT('?'), QueryIndex))
        {
            Path.LeftInline(QueryIndex);
        }

        const int32 PrefixLength = FCString::Strlen(PathPrefix);
        const int32 SuffixLength = FCString::Strlen(PathSuffix);

        if (Path.Len() > PrefixLength + SuffixLength && Path.StartsWith(PathPrefix) && Path.EndsWith(PathSuffix))
        {
            Identity.ClassName = Path.Mid(PrefixLength, Path.Len() - PrefixLength - SuffixLength);
        }
    }

    return Identity;
}
} // namespace

namespace dap::Extensions
{
template <typename Request, typename Class, typename Response>
static void Register(const TSharedPtr<dap::Session>& Session, const TWeakPtr<Class>& WeakObject, Response (Class::*Handler)(const Request&))
{
    UE_LOG(LogBlueprintInspector, Verbose, TEXT("Registering handler for %hs"), typeid(Request).name());
    Session->registerHandler([WeakObject, Handler](const Request& InRequest) -> Response {
			TSharedPtr<Class> Object = WeakObject.Pin();
			if (!Object)
			{
				return dap::Error("BlueprintInspector is shutting down");
			}

			UE_LOG(LogBlueprintInspector, Log, TEXT("DAP request: %hs"), typeid(Request).name());
			try
			{
				return (Object.Get()->*Handler)(InRequest);
			}
			catch (const std::exception& Exception)
			{
				return dap::Error("DAP handler failed: %s", Exception.what());
			}
			catch (...)
			{
				return dap::Error("DAP handler failed");
			} });
}

// As Register, for a sent handler that talks back to the session that sent the request. The session is
// captured at registration so a reconnect cannot redirect the follow-up event to another connection.
template <typename ResponseType, typename Class>
static void RegisterSentForSession(const TSharedPtr<dap::Session>& Session, const TWeakPtr<Class>& WeakObject, void (Class::*Handler)(const dap::ResponseOrError<ResponseType>&, const TSharedPtr<dap::Session>&))
{
    TWeakPtr<dap::Session> WeakSession = Session;
    Session->registerSentHandler([WeakObject, WeakSession, Handler](const dap::ResponseOrError<ResponseType>& InResponse) {
			if (TSharedPtr<Class> Object = WeakObject.Pin())
			{
				if (TSharedPtr<dap::Session> StrongSession = WeakSession.Pin())
				{
					(Object.Get()->*Handler)(InResponse, StrongSession);
				}
			} });
}

// As Register, for a handler that makes an auth decision: it receives the generation of the connection
// that registered it, so a request delivered late cannot act on a newer connection's auth state.
template <typename Request, typename Class, typename Response>
static void RegisterWithGeneration(const TSharedPtr<dap::Session>& Session, const TWeakPtr<Class>& WeakObject, const uint64 Generation, Response (Class::*Handler)(const Request&, uint64))
{
    Session->registerHandler([WeakObject, Handler, Generation](const Request& InRequest) -> Response {
			TSharedPtr<Class> Object = WeakObject.Pin();
			if (!Object)
			{
				return dap::Error("BlueprintInspector is shutting down");
			}

			try
			{
				return (Object.Get()->*Handler)(InRequest, Generation);
			}
			catch (const std::exception& Exception)
			{
				return dap::Error("DAP handler failed: %s", Exception.what());
			}
			catch (...)
			{
				return dap::Error("DAP handler failed");
			} });
}

// Handlers that reveal or mutate debugger state are held until the client presents the session token.
// The request is dispatched from here when it is allowed, so this uses cppdap's callback form.
template <typename Request, typename Class, typename Response>
static void RegisterGated(const TSharedPtr<dap::Session>& Session, const TWeakPtr<Class>& WeakObject, const uint64 Generation, Response (Class::*Handler)(const Request&))
{
    using ResponseValue = typename Response::Request;

    Session->registerHandler([WeakObject, Handler, Generation](const Request& InRequest, const std::function<void(dap::ResponseOrError<ResponseValue>)>& Respond) {
        TSharedPtr<Class> Object = WeakObject.Pin();
        if (!Object)
        {
            Respond(dap::Error("BlueprintInspector is shutting down"));
            return;
        }

        UE_LOG(LogBlueprintInspector, Log, TEXT("DAP request (gated): %hs"), typeid(Request).name());
        TSharedPtr<Request> RequestPtr = MakeShared<Request>(InRequest);
        Object->DispatchAuthenticatedForGeneration(Generation, [Object, Handler, RequestPtr, Respond](const bool bAllowed)
        {
            if (!bAllowed)
            {
                Respond(dap::Error("Unauthorized: the DAP session is not authenticated"));
                return;
            }

            try
            {
                Respond((Object.Get()->*Handler)(*RequestPtr));
            }
            catch (const std::exception& Exception)
            {
                Respond(dap::Error("DAP handler failed: %s", Exception.what()));
            }
            catch (...)
            {
                Respond(dap::Error("DAP handler failed"));
            }
        }); });
}

// As RegisterGated, for handlers that must run off cppdap's dispatch thread.
template <typename Request, typename Class, typename Response>
static void RegisterGatedAsync(const TSharedPtr<dap::Session>& Session, const TWeakPtr<Class>& WeakObject, const uint64 Generation, Response (Class::*Handler)(const Request&))
{
    using ResponseValue = typename Response::Request;

    TWeakPtr<dap::Session> WeakSession = Session;
    Session->registerHandler([WeakObject, WeakSession, Handler, Generation](const Request& InRequest, const std::function<void(dap::ResponseOrError<ResponseValue>)>& Respond) {
        TSharedPtr<Class> Object = WeakObject.Pin();
        TSharedPtr<dap::Session> StrongSession = WeakSession.Pin();
        if (!Object || !StrongSession)
        {
            Respond(dap::Error("BlueprintInspector is shutting down"));
            return;
        }

        UE_LOG(LogBlueprintInspector, Log, TEXT("DAP request (gated async): %hs"), typeid(Request).name());
        TSharedPtr<Request> RequestPtr = MakeShared<Request>(InRequest);
        Object->DispatchAuthenticatedForGeneration(Generation, [Object = MoveTemp(Object), StrongSession = MoveTemp(StrongSession), Handler, RequestPtr, Respond](const bool bAllowed)
        {
            if (!bAllowed)
            {
                Respond(dap::Error("Unauthorized: the DAP session is not authenticated"));
                return;
            }

            Async(EAsyncExecution::ThreadPool, [Object = Object, StrongSession = StrongSession, Handler, RequestPtr, Respond]() mutable noexcept
                  {
                try
                {
                    Respond((Object.Get()->*Handler)(*RequestPtr));
                }
                catch (const std::exception& Exception)
                {
                    try
                    {
                        Respond(dap::Error("DAP handler failed: %s", Exception.what()));
                    }
                    catch (...)
                    {
                        UE_LOG(LogBlueprintInspector, Warning, TEXT("Failed to send the DAP error response (handler: %hs)"), typeid(Request).name());
                        return;
                    }
                }
                catch (...)
                {
                    try
                    {
                        Respond(dap::Error("DAP handler failed"));
                    }
                    catch (...)
                    {
                        UE_LOG(LogBlueprintInspector, Warning, TEXT("Failed to send the DAP error response (handler: %hs)"), typeid(Request).name());
                        return;
                    }
                } });
        }); });
}

} // namespace dap::Extensions

void FUBlueprintInspectorDap::SetCallbacks(const FBlueprintInspectorDapCallbacks& InCallbacks)
{
    TSharedPtr<FBlueprintInspectorCallbackGate, ESPMode::ThreadSafe> Previous;

    {
        FScopeLock Lock(&CallbackMutex);
        Callbacks = InCallbacks;
        Previous = MoveTemp(CallbackGate);
        CallbackGate = MakeShared<FBlueprintInspectorCallbackGate, ESPMode::ThreadSafe>();
    }

    // Close the gate that was installed before: a handler holding a copy of the previous bundle must not
    // stay live after this replacement. Done outside the lock so a handler already running can observe it.
    if (Previous.IsValid())
    {
        Previous->bOpen.store(false);
    }
}

void FUBlueprintInspectorDap::ClearCallbacks()
{
    TSharedPtr<FBlueprintInspectorCallbackGate, ESPMode::ThreadSafe> Gate;
    {
        FScopeLock Lock(&CallbackMutex);
        Callbacks = {};
        Gate = MoveTemp(CallbackGate);
    }

    // Closed outside the lock: a handler already holding a copy of a callback must observe the closed
    // gate before the subsystem finishes tearing down.
    if (Gate.IsValid())
    {
        Gate->bOpen.store(false);
    }
}

namespace
{
constexpr int32 DefaultListeningPort = 19021;
constexpr int32 MaxTcpPort = 65535;
constexpr int32 TokenPublishAttempts = 8;
constexpr float TokenPublishRetrySeconds = 0.05f;
} // namespace

int32 FUBlueprintInspectorDap::ListeningPort()
{
    // Parsed once: the suite reads the same value back from the command line in its client code, so the
    // override must not change under a running session.
    static const int32 Port = []() {
        int32 Override = 0;
        if (FParse::Value(FCommandLine::Get(), TEXT("BlueprintInspectorDapPort="), Override) && Override > 0 && Override <= MaxTcpPort)
        {
            return Override;
        }
        return DefaultListeningPort;
    }();
    return Port;
}

namespace
{
/** "dap-token", or "dap-token-<port>" when the listener port was overridden. */
FString TokenFileName()
{
    const int32 Port = FUBlueprintInspectorDap::ListeningPort();
    return Port == DefaultListeningPort ? FString(TEXT("dap-token")) : FString::Printf(TEXT("dap-token-%d"), Port);
}

// Publishes a file by writing a sibling temporary and renaming it into place. A client polling the path
// therefore sees the previous complete contents or the new complete contents, never a truncated write.
bool WriteFileAtomically(const FString& Path, const FString& Contents)
{
    const FString TempPath = Path + FString::Printf(TEXT(".tmp-%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);

    if (!FFileHelper::SaveStringToFile(Contents, *TempPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        IFileManager::Get().Delete(*TempPath, false, true, true);
        return false;
    }

    for (int32 Attempt = 0; Attempt < TokenPublishAttempts; ++Attempt)
    {
        if (IFileManager::Get().Move(*Path, *TempPath, true, true))
        {
            return true;
        }
        FPlatformProcess::Sleep(TokenPublishRetrySeconds);
    }

    IFileManager::Get().Delete(*TempPath, false, true, true);
    return false;
}
} // namespace

// Two copies by decision: the project file is what the editor and a workspace opened at the project
// find, and the per-user file covers a packaged game staged outside the project. The extension tries
// `blueprintInspector.tokenFile`, then the per-user file, then the project file and one level below it;
// README.md states that order, so a change here has to change the README with it.
FString FUBlueprintInspectorDap::GetTokenFilePath()
{
    return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("BlueprintInspector"), TokenFileName());
}

FString FUBlueprintInspectorDap::GetSharedTokenFilePath()
{
    // Only one process can own the listening port, so a per-user file is unambiguous. It also stays
    // discoverable for a packaged game, whose Saved directory is wherever the build was staged. When the
    // port is overridden the name carries it, so a suite run cannot clear a live session's token.
    return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("BlueprintInspector"), TokenFileName());
}

void FUBlueprintInspectorDap::WriteTokenFile(const FString& Token)
{
    // Publish the per-user copy first: the extension tries it before the project copy, so the
    // higher-priority location must be current first. Each copy is a rename, so the window between them
    // exposes only a complete (older) token, never an empty or partial one.
    const FString SharedPath = GetSharedTokenFilePath();
    if (!WriteFileAtomically(SharedPath, Token))
    {
        // Never leave a stale higher-priority token behind: the client reads that location first and
        // would authenticate against the previous connection's token.
        IFileManager::Get().Delete(*SharedPath, false, true, true);
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Could not write the DAP session token to %s"), *SharedPath);
    }

    const FString ProjectPath = GetTokenFilePath();
    if (!WriteFileAtomically(ProjectPath, Token))
    {
        IFileManager::Get().Delete(*ProjectPath, false, true, true);
        UE_LOG(LogBlueprintInspector, Error, TEXT("Could not write the DAP session token to %s"), *ProjectPath);
    }
}

void FUBlueprintInspectorDap::DeleteTokenFile()
{
    IFileManager::Get().Delete(*GetTokenFilePath(), false, true, true);
    IFileManager::Get().Delete(*GetSharedTokenFilePath(), false, true, true);
}

void FUBlueprintInspectorDap::DeleteTokenFileIfMatches(const FString& ExpectedToken)
{
    const FString Paths[] = {GetTokenFilePath(), GetSharedTokenFilePath()};
    for (const FString& Path : Paths)
    {
        FString OnDisk;
        if (FFileHelper::LoadFileToString(OnDisk, *Path) && OnDisk.TrimStartAndEnd().Equals(ExpectedToken, ESearchCase::CaseSensitive))
        {
            IFileManager::Get().Delete(*Path, false, true, true);
        }
    }
}

bool FUBlueprintInspectorDap::IsAuthenticated() const
{
    return Auth.IsAuthenticated();
}

void FUBlueprintInspectorDap::DispatchAuthenticatedForGeneration(const uint64 Generation, const TFunction<void(bool)>& Work)
{
    uint64 TimerGeneration = 0;
    uint64 TimerBatch = 0;
    const FSessionAuth::EQueueResult Result = Auth.Queue(Generation, Work, TimerGeneration, TimerBatch);
    if (Result == FSessionAuth::EQueueResult::Allowed)
    {
        Work(true);
        return;
    }

    if (Result == FSessionAuth::EQueueResult::Rejected)
    {
        Work(false);
        return;
    }

    if (Result == FSessionAuth::EQueueResult::QueuedWithTimer)
    {
        // The timer below runs on the pool, so bind it to the connection it is arming. Without this it
        // can fire after that socket was dropped and reject a newer live connection.
        TWeakPtr<FUBlueprintInspectorDap> WeakSelf = AsShared();
        Async(EAsyncExecution::ThreadPool, [WeakSelf, TimerGeneration, TimerBatch]() {
			FPlatformProcess::Sleep(static_cast<float>(AuthTimeoutSeconds));
			if (TSharedPtr<FUBlueprintInspectorDap> Self = WeakSelf.Pin())
			{
				Self->TimeoutPendingRequests(TimerGeneration, TimerBatch);
			} });
    }
}

void FUBlueprintInspectorDap::FlushPendingRequests(const bool bAllowed)
{
    TArray<TFunction<void(bool)>> Pending = Auth.Drain();

    for (TFunction<void(bool)>& Work : Pending)
    {
        if (Work)
        {
            Work(bAllowed);
        }
    }
}

void FUBlueprintInspectorDap::TimeoutPendingRequests(const uint64 Generation, const uint64 Batch)
{
    TArray<TFunction<void(bool)>> Pending;
    if (!Auth.DrainIfExpired(Generation, Batch, Pending))
    {
        return;
    }

    UE_LOG(LogBlueprintInspector, Warning, TEXT("DAP client did not authenticate within %d s; rejecting queued requests and closing the connection"), AuthTimeoutSeconds);
    for (TFunction<void(bool)>& Work : Pending)
    {
        if (Work)
        {
            Work(false);
        }
    }
    CloseClientSocket(Generation);
}

void FUBlueprintInspectorDap::CloseIfUnauthenticated(const uint64 Generation)
{
    const bool bClose = Auth.IsUnauthenticatedFor(Generation);

    if (bClose)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("DAP client did not authenticate within %d s; closing the connection"), AuthTimeoutSeconds);
        CloseClientSocket(Generation);
    }
}

void FUBlueprintInspectorDap::CloseClientSocket(const uint64 Generation)
{
    std::shared_ptr<dap::ReaderWriter> Socket;
    {
        FScopeLock Lock(&SessionMutex);
        // The decision was made for one connection; a close must never reach the one that replaced it.
        Socket = Published.SocketForClose(Generation);
    }

    if (Socket)
    {
        Socket->close();
    }
}

bool FUBlueprintInspectorDap::Authenticate(const FString& Provided, const uint64 Generation)
{
    int32 Attempts = 0;
    uint64 AuthGeneration = 0;
    const FSessionAuth::EHandshakeResult Result = Auth.Authenticate(Provided, Generation, Attempts, AuthGeneration);
    if (Result == FSessionAuth::EHandshakeResult::StaleGeneration)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Rejected a DAP handshake from a retired connection"));
        return false;
    }
    if (Result == FSessionAuth::EHandshakeResult::Rejected)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Rejected a DAP session token"));
        FlushPendingRequests(false);
        if (Attempts >= MaxAuthAttempts)
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Closing the DAP connection after %d rejected tokens"), MaxAuthAttempts);
            CloseClientSocket(AuthGeneration);
        }
        return false;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("DAP client authenticated"));
    FlushPendingRequests(true);
    return true;
}

bool FUBlueprintInspectorDap::StartSession()
{
    {
        FScopeLock Lock(&SessionMutex);
        if (Server || Published.IsPublished() || bStarting)
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("DAP session already exists; ignoring StartSession"));
            return Server != nullptr;
        }
        bStarting = true;
        bShuttingDown = false;
    }

    TUniquePtr<dap::net::Server> NewServer(dap::net::Server::create().release());
    if (!NewServer)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("Failed to create the DAP server"));
        FScopeLock Lock(&SessionMutex);
        bStarting = false;
        return false;
    }

    TWeakPtr<FUBlueprintInspectorDap> WeakSelf = AsShared();
    const bool bStarted = NewServer->start("127.0.0.1", ListeningPort(), [WeakSelf](const std::shared_ptr<dap::ReaderWriter>& Socket) {
			if (TSharedPtr<FUBlueprintInspectorDap> Self = WeakSelf.Pin())
			{
				Self->OnClientConnected(Socket);
			} }, [](const char* Msg) { UE_LOG(LogBlueprintInspector, Warning, TEXT("DAP server: %s"), *FString(ANSI_TO_TCHAR(Msg))); });

    if (!bStarted)
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("DAP server failed to listen on 127.0.0.1:%d"), ListeningPort());
        FScopeLock Lock(&SessionMutex);
        bStarting = false;
        return false;
    }

    bool bPublished = false;
    {
        FScopeLock Lock(&SessionMutex);
        bStarting = false;
        if (bShuttingDown)
        {
            // EndSession ran while the listener was starting. Do not publish it: EndSession has already
            // taken its snapshot and cannot stop a server it never saw.
            UE_LOG(LogBlueprintInspector, Warning, TEXT("DAP server started during shutdown; not publishing it"));
        }
        else
        {
            Server = MoveTemp(NewServer);
            bPublished = true;
        }
    }

    if (!bPublished)
    {
        NewServer->stop();
        return false;
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("DAP server listening on 127.0.0.1:%d"), ListeningPort());
    return true;
}

TSharedRef<FUBlueprintInspectorDap> FUBlueprintInspectorDap::Create()
{
    // MakeShared cannot construct an object with a private constructor (its reference controller is a
    // separate template), so allocate here, where the access is legal, and hand the pointer to the
    // shared-pointer helper.
    return MakeShareable(new FUBlueprintInspectorDap());
}

FUBlueprintInspectorDap::~FUBlueprintInspectorDap()
{
    // Normal teardown is EndSession(), which stops the listener and hands the session to a detached
    // thread. Anything left here is a lifecycle bug; at least make sure the port cannot stay open after
    // the owner is gone.
    if (Server || Published.IsPublished())
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("DAP adapter destroyed with a live server or session; EndSession was not called"));
        if (Server)
        {
            Server->stop();
        }
    }
    Server.Reset();

    // Destroying a cppdap session joins its receive and dispatch threads, and a dispatch handler may be
    // waiting on a game-thread task, so doing that inline here (on the game thread) can deadlock. Use
    // the same hand-off as EndSession; the task owns the last reference and needs no `this`.
    TSharedPtr<dap::Session> Retired = Published.RetireAll();
    if (Retired.IsValid())
    {
        Async(EAsyncExecution::ThreadPool, [Retired = MoveTemp(Retired)]() mutable { Retired.Reset(); });
    }
}

void FUBlueprintInspectorDap::EndSession()
{
    const FContinueExecution Continue = GetCallback(&FBlueprintInspectorDapCallbacks::ContinueExecution);
    if (Continue)
    {
        Continue();
    }

    TUniquePtr<dap::net::Server> OldServer;
    TSharedPtr<dap::Session> OldSession;
    uint64 RetiredGeneration = 0;
    {
        FScopeLock Lock(&SessionMutex);
        bShuttingDown = true;
        RetiredGeneration = Published.GetGeneration();
        OldServer = MoveTemp(Server);
        OldSession = Published.RetireAll();
    }

    if (OldServer)
    {
        OldServer->stop();
    }

    if (const FCaptureControl Clear = GetCallback(&FBlueprintInspectorDapCallbacks::ClearCapture))
    {
        Clear();
    }

    ClearCallbacks();

    if (RetiredGeneration != 0)
    {
        const FString ClearedToken = Auth.Reset(RetiredGeneration);
        if (!ClearedToken.IsEmpty())
        {
            DeleteTokenFileIfMatches(ClearedToken);
        }
    }
    else
    {
        Auth.ResetAll();
        DeleteTokenFile();
    }

    if (OldSession)
    {
        // Destroying a cppdap session joins its receive and dispatch threads. Do that off the game
        // thread: a dispatch handler may be waiting on a game-thread task.
        Async(EAsyncExecution::ThreadPool, [OldSession = MoveTemp(OldSession)]() mutable { OldSession.Reset(); });
    }
}

TSharedPtr<dap::Session> FUBlueprintInspectorDap::GetSession() const
{
    FScopeLock Lock(&SessionMutex);
    return Published.GetSession();
}

FUBlueprintInspectorDap::FSessionSnapshot FUBlueprintInspectorDap::GetSessionSnapshot() const
{
    FScopeLock Lock(&SessionMutex);
    FSessionSnapshot Snapshot;
    Snapshot.Session = Published.GetSession();
    Snapshot.Generation = Published.GetGeneration();
    return Snapshot;
}

bool FUBlueprintInspectorDap::HasSession() const
{
    return GetSession() != nullptr;
}

void FUBlueprintInspectorDap::SendStoppedEvent(const FString& Reason, const int32 ThreadId, const FString& Text, const FString& Description)
{
    // Snapshot the published session and its generation together: two separate reads could straddle a
    // reconnect and deliver paused Blueprint state to a client that never presented a token.
    const FSessionSnapshot Snapshot = GetSessionSnapshot();
    if (!Snapshot.Session.IsValid() || !Auth.IsAuthenticatedFor(Snapshot.Generation))
    {
        return;
    }

    dap::StoppedEvent Event;
    Event.reason = TCHAR_TO_UTF8(*Reason);
    Event.threadId = ThreadId;
    Event.allThreadsStopped = true;
    if (!Text.IsEmpty())
    {
        Event.text = TCHAR_TO_UTF8(*Text);
    }
    if (!Description.IsEmpty())
    {
        Event.description = TCHAR_TO_UTF8(*Description);
    }
    Snapshot.Session->send(Event);
}

void FUBlueprintInspectorDap::OnClientConnected(const std::shared_ptr<dap::ReaderWriter>& Socket)
{
    {
        FScopeLock Lock(&SessionMutex);
        if (bShuttingDown || Published.IsPublished() || ConnectingSlot.IsConnecting())
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Rejected a second DAP client"));
            if (Socket)
            {
                Socket->close();
            }
            return;
        }
        // Claim the slot for the whole setup. Publishing only after bind means a teardown or a stopped
        // event can never observe a session whose handlers are not registered yet.
        ConnectingSlot.Begin();
    }

    uint64 Generation = 0;
    bool bPublished = false;

    // Session creation and handler registration can throw. Finish() clears the slot on the publication
    // path; this guard covers every other exit, so a failed setup cannot leave the listener permanently
    // reporting "already connected".
    ON_SCOPE_EXIT
    {
        bool bAbandoned = false;
        {
            FScopeLock Lock(&SessionMutex);
            bAbandoned = !bPublished && Generation != 0 && ConnectingSlot.IsFor(Generation);
            if (bAbandoned)
            {
                ConnectingSlot.Abort();
            }
        }
        if (bAbandoned)
        {
            if (Socket)
            {
                Socket->close();
            }
            const FString ClearedToken = Auth.Reset(Generation);
            if (!ClearedToken.IsEmpty())
            {
                DeleteTokenFileIfMatches(ClearedToken);
            }
        }
    };

    TSharedPtr<dap::Session> NewSession(dap::Session::create().release());
    if (!NewSession.IsValid())
    {
        {
            FScopeLock Lock(&SessionMutex);
            ConnectingSlot.Abort();
        }
        if (Socket)
        {
            Socket->close();
        }
        return;
    }

    NewSession->setOnInvalidData(dap::kClose);
    NewSession->onError([](const char* Msg) { UE_LOG(LogBlueprintInspector, Log, TEXT("%s"), *FString(ANSI_TO_TCHAR(Msg))); });

    // A token per connection, published before the receive thread starts so the extension can read it
    // as soon as the session exists. The generation ties the token and auth state to this accept, so a
    // late disconnect from an older connection cannot clear a newer connection's token.
    const FString Token = Auth.Begin(Generation);
    WriteTokenFile(Token);

    {
        // Publish the generation before bind starts the receive thread. A disconnect that lands while
        // the session is still being prepared compares against this value and marks the accept as
        // dead, so publication cannot claim the slot with a closed socket.
        FScopeLock Lock(&SessionMutex);
        ConnectingSlot.SetGeneration(Generation);
    }

    TWeakPtr<FUBlueprintInspectorDap> WeakSelf = AsShared();
    dap::Extensions::Register(NewSession, WeakSelf, &FUBlueprintInspectorDap::InitializeRequest);
    dap::Extensions::RegisterSentForSession(NewSession, WeakSelf, &FUBlueprintInspectorDap::InitializeResponse);

    dap::Extensions::Register(NewSession, WeakSelf, &FUBlueprintInspectorDap::AttachRequest);
    dap::Extensions::RegisterWithGeneration(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::LaunchRequest);
    dap::Extensions::Register(NewSession, WeakSelf, &FUBlueprintInspectorDap::ThreadsRequest);
    dap::Extensions::RegisterGatedAsync(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::LoadedSourcesRequest);
    dap::Extensions::RegisterGatedAsync(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::BreakpointLocationsRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::StackTraceRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::ScopesRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::VariablesRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::SetVariableRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::EvaluateRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::PauseRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::ContinueRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::NextRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::StepInRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::StepOutRequest);
    dap::Extensions::RegisterGatedAsync(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::SetBreakpointsRequest);
    dap::Extensions::RegisterGatedAsync(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::SourceRequest);
    dap::Extensions::RegisterWithGeneration(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::DisconnectRequest);
    dap::Extensions::Register(NewSession, WeakSelf, &FUBlueprintInspectorDap::ConfigurationDoneRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::StartCaptureRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::StopCaptureRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::ResetCaptureRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::CaptureStateRequest);
    dap::Extensions::RegisterGatedAsync(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::HotLinesRequest);
    dap::Extensions::RegisterGatedAsync(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::CoverageRequest);
    dap::Extensions::RegisterGatedAsync(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::AuditRequest);
    dap::Extensions::RegisterGated(NewSession, WeakSelf, Generation, &FUBlueprintInspectorDap::SetExceptionBreakpointsRequest);

    NewSession->bind(Socket, [WeakSelf, WeakSession = TWeakPtr<dap::Session>(NewSession), Generation]() {
		if (TSharedPtr<FUBlueprintInspectorDap> Self = WeakSelf.Pin())
		{
			UE_LOG(LogBlueprintInspector, Log, TEXT("DAP client disconnected"));
			Self->OnClientDisconnected(WeakSession, Generation);
		} });

    bool bAcceptAborted = false;
    {
        FScopeLock Lock(&SessionMutex);
        bAcceptAborted = ConnectingSlot.Finish();
        if (bAcceptAborted || bShuttingDown || Published.IsPublished())
        {
            UE_LOG(LogBlueprintInspector, Warning, TEXT("Rejected DAP client: %s"), bAcceptAborted ? TEXT("the socket closed before publication") : (bShuttingDown ? TEXT("adapter is shutting down") : TEXT("already connected")));
        }
        else
        {
            Published.Publish(NewSession, Socket, Generation);
            bPublished = true;
        }
    }

    if (!bPublished)
    {
        if (Socket)
        {
            // Bound but never published. Closing ends the receive thread; this (accept) thread may
            // safely release the session when the function returns.
            Socket->close();
        }

        const FString ClearedToken = Auth.Reset(Generation);
        if (!ClearedToken.IsEmpty())
        {
            DeleteTokenFileIfMatches(ClearedToken);
        }
        return;
    }

    // A client that never presents a token must not hold the single session slot forever.
    Async(EAsyncExecution::ThreadPool, [WeakSelf, Generation]() {
              FPlatformProcess::Sleep(static_cast<float>(AuthTimeoutSeconds));
              if (TSharedPtr<FUBlueprintInspectorDap> Self = WeakSelf.Pin())
              {
                  Self->CloseIfUnauthenticated(Generation);
              } });
}

void FUBlueprintInspectorDap::OnClientDisconnected(const TWeakPtr<dap::Session>& WeakSession, const uint64 Generation)
{
    // This runs on cppdap's receive thread. Clear the slot first so a new breakpoint cannot pause
    // against a dead client, then resume the game thread if it is parked. Do not destroy the
    // session here, because its destructor joins this thread.
    TSharedPtr<dap::Session> Retired;
    {
        FScopeLock Lock(&SessionMutex);
        // Pin() stays valid while OnClientConnected still holds its local session, so a close before
        // publication reaches this branch and Retire() finds nothing published. Record it on the accept
        // slot as well: a pre-publication close must not be published as a live connection.
        const TSharedPtr<dap::Session> ClosedSession = WeakSession.Pin();
        if (ClosedSession.IsValid())
        {
            // Retires only when the generation that closed still owns the slot; a late callback from an
            // older connection cannot drop the one that replaced it.
            Retired = Published.Retire(Generation);
        }

        if (!Retired.IsValid() && ConnectingSlot.IsFor(Generation))
        {
            // The socket dropped while its session was still being prepared. OnClientConnected will
            // see this and abort publication instead of occupying the slot with a dead connection.
            ConnectingSlot.MarkClosed(Generation);
        }
    }

    if (!Retired.IsValid())
    {
        return;
    }

    if (const FCaptureControl Clear = GetCallback(&FBlueprintInspectorDapCallbacks::ClearCapture))
    {
        Clear();
    }

    // Only an authenticated connection can have parked the VM. Capture the state for *this*
    // generation before the session is torn down: checking the shared flag afterwards could observe a
    // newer connection (or one that already disconnected) and either resume a VM nobody parked or
    // leave a parked one behind. EndSession() is the explicit host teardown and always resumes.
    const bool bWasAuthenticated = Auth.IsAuthenticatedFor(Generation);

    if (bWasAuthenticated)
    {
        const FContinueExecution Continue = GetCallback(&FBlueprintInspectorDapCallbacks::ContinueExecution);
        if (Continue)
        {
            Continue();
        }
    }

    const FString ClearedToken = Auth.Reset(Generation);
    if (!ClearedToken.IsEmpty())
    {
        // A newer accept may already have written its token; only remove this connection's file.
        DeleteTokenFileIfMatches(ClearedToken);
    }

    Async(EAsyncExecution::ThreadPool, [Retired = MoveTemp(Retired)]() mutable { Retired.Reset(); });
}

void FUBlueprintInspectorDap::SendOutputEvent(const FString& Output)
{
    const FSessionSnapshot Snapshot = GetSessionSnapshot();
    if (!Snapshot.Session.IsValid() || !Auth.IsAuthenticatedFor(Snapshot.Generation))
    {
        return;
    }

    dap::OutputEvent Event;
    Event.category = "console";
    Event.output = TCHAR_TO_UTF8(*Output);
    Snapshot.Session->send(Event);
}

dap::ResponseOrError<dap::InitializeResponse> FUBlueprintInspectorDap::InitializeRequest(const dap::InitializeRequest& Request)
{
    if (Request.adapterID != DebugType)
    {
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Rejected DAP initialize with adapterID '%s'"), UTF8_TO_TCHAR(Request.adapterID.c_str()));
        return dap::Error("Unauthorized: invalid adapterID");
    }

    dap::InitializeResponse Response;
    Response.supportsConfigurationDoneRequest = true;
    Response.supportsLoadedSourcesRequest = true;
    Response.supportsBreakpointLocationsRequest = true;
    Response.supportsSetVariable = true;
    Response.supportsEvaluateForHovers = true;
    Response.supportsConditionalBreakpoints = true;
    Response.supportsHitConditionalBreakpoints = true;
    Response.supportsLogPoints = true;

    // Filters are opt-in: without one selected an attached client changes nothing about how a runtime
    // error reports, which is what the log showed before the plugin was installed.
    dap::array<dap::ExceptionBreakpointsFilter> ExceptionFilters;
    dap::ExceptionBreakpointsFilter AccessedNone;
    AccessedNone.filter = GetExceptionFilterName(EBlueprintExceptionFilter::AccessedNone);
    AccessedNone.label = "Accessed None";
    AccessedNone.description = "Stop where a null object, a missing property or an out-of-range array access raises a Blueprint access violation.";
    AccessedNone.def = false;
    ExceptionFilters.push_back(AccessedNone);

    dap::ExceptionBreakpointsFilter RuntimeError;
    RuntimeError.filter = GetExceptionFilterName(EBlueprintExceptionFilter::RuntimeError);
    RuntimeError.label = "Blueprint runtime errors";
    RuntimeError.description = "Stop on the VM's non-fatal and fatal script errors.";
    RuntimeError.def = false;
    ExceptionFilters.push_back(RuntimeError);

    dap::ExceptionBreakpointsFilter InfiniteLoop;
    InfiniteLoop.filter = GetExceptionFilterName(EBlueprintExceptionFilter::InfiniteLoop);
    InfiniteLoop.label = "Runaway loops";
    InfiniteLoop.description = "Stop when the VM aborts a loop that exceeded its iteration limit.";
    InfiniteLoop.def = false;
    ExceptionFilters.push_back(InfiniteLoop);

    Response.exceptionBreakpointFilters = ExceptionFilters;
    return Response;
}

void FUBlueprintInspectorDap::InitializeResponse(const dap::ResponseOrError<dap::InitializeResponse>& Response, const TSharedPtr<dap::Session>& Session)
{
    if (Response.error)
    {
        // A rejected initialize must not be followed by `initialized`: the client would proceed into a
        // session the adapter refused.
        UE_LOG(LogBlueprintInspector, Warning, TEXT("Not sending initialized event: initialize failed: %s"), *FString(ANSI_TO_TCHAR(Response.error.message.c_str())));
        return;
    }

    // Sent on the session that sent initialize: looking up the current one could deliver `initialized`
    // to a connection that never asked for it.
    if (Session.IsValid())
    {
        Session->send(dap::InitializedEvent());
    }
}

dap::ResponseOrError<dap::LaunchResponse> FUBlueprintInspectorDap::LaunchRequest(const dap::LaunchRequest& Request, const uint64 Generation)
{
    // VS Code's debugServer handshake fixes adapterID and sends no launch arguments, so the extension
    // presents the session token in a second launch request's __restart payload.
    if (Request.restart.has_value() && Request.restart->is<dap::object>())
    {
        const dap::object& Restart = Request.restart->get<dap::object>();
        const auto TokenIt = Restart.find("token");
        if (TokenIt != Restart.end() && TokenIt->second.is<dap::string>())
        {
            const dap::string& Token = TokenIt->second.get<dap::string>();
            if (!Authenticate(UTF8_TO_TCHAR(Token.c_str()), Generation))
            {
                return dap::Error("Unauthorized: invalid session token");
            }
        }
    }

    dap::LaunchResponse Response;
    return Response;
}

dap::ResponseOrError<dap::ThreadsResponse> FUBlueprintInspectorDap::ThreadsRequest(const dap::ThreadsRequest&)
{
    dap::ThreadsResponse Response;
    dap::Thread Thread;
    Thread.id = 1;
    Thread.name = "Blueprint Inspector";
    Response.threads.push_back(Thread);
    return Response;
}

dap::ResponseOrError<dap::AttachResponse> FUBlueprintInspectorDap::AttachRequest(const dap::AttachRequest&)
{
    dap::AttachResponse Response;
    return Response;
}

dap::ResponseOrError<dap::LoadedSourcesResponse> FUBlueprintInspectorDap::LoadedSourcesRequest(const dap::LoadedSourcesRequest&)
{
    dap::LoadedSourcesResponse Response;

    const FAcquireClassListDelegate AcquireClassList = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireClassList);
    if (AcquireClassList)
    {
        const TGameThreadCallResult<TArray<FFunctionProperties>> Result = AcquireClassList();
        if (Result.bTimedOut)
        {
            return dap::Error("The game thread did not service the loadedSources request");
        }

        const TArray<FFunctionProperties>& Functions = Result.Value;
        for (const FFunctionProperties& Function : Functions)
        {
            const FString Name = Function.Name.ToString();
            const FString Path = FString::Printf(TEXT("blueprint-source:Blueprints/%s.cpp"), *Name);

            dap::Source Source;
            Source.name = TCHAR_TO_UTF8(*Name);
            Source.path = TCHAR_TO_UTF8(*Path);
            Source.sourceReference = Function.ID;
            Response.sources.push_back(Source);
        }
    }

    return Response;
}

dap::ResponseOrError<dap::BreakpointLocationsResponse> FUBlueprintInspectorDap::BreakpointLocationsRequest(const dap::BreakpointLocationsRequest& Request)
{
    const FBlueprintSourceIdentity Identity = DescribeIdentity(Request.source);
    const int64 RequestedFirstLine = FMath::Max<int64>(1, Request.line);
    const int64 RequestedLastLine = Request.endLine.has_value() ? static_cast<int64>(Request.endLine.value()) : RequestedFirstLine;

    UE_LOG(LogBlueprintInspector, Log, TEXT("breakpointLocations: %s lines %lld-%lld"), *DescribeSource(Request.source), RequestedFirstLine, RequestedLastLine);

    const FAcquireSourceMap AcquireMap = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireSourceMap);
    if (!AcquireMap)
    {
        return dap::Error("BlueprintInspector has no source map for that reference");
    }

    const TGameThreadCallResult<FBlueprintSourceMap> MapResult = AcquireMap(Identity);
    if (MapResult.bTimedOut)
    {
        return dap::Error("The game thread did not service the source map request");
    }
    const FBlueprintSourceMap& Map = MapResult.Value;

    dap::BreakpointLocationsResponse Response;
    const int32 MaxLine = Map.Lines.Num();
    if (MaxLine > 0)
    {
        // A client-supplied endLine is untrusted; clamp it to the rendered map before iterating.
        const int32 FirstLine = static_cast<int32>(FMath::Min<int64>(FMath::Max<int64>(RequestedFirstLine, 1), MaxLine));
        const int32 LastLine = static_cast<int32>(FMath::Min<int64>(FMath::Max<int64>(RequestedLastLine, 0), MaxLine));
        for (int32 Line = FirstLine; Line <= LastLine; ++Line)
        {
            if (!Map.IsBreakpointable(Line))
            {
                continue;
            }

            dap::BreakpointLocation Location;
            Location.line = Line;
            Response.breakpoints.push_back(Location);
        }
    }

    return Response;
}

dap::ResponseOrError<dap::StackTraceResponse> FUBlueprintInspectorDap::StackTraceRequest(const dap::StackTraceRequest& Request)
{
    const FAcquireStackFrames AcquireFrames = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireStackFrames);
    if (!AcquireFrames)
    {
        return dap::Error("BlueprintInspector has no paused frame");
    }

    dap::StackTraceResponse Response;
    const TArray<FBlueprintStackFrame> Frames = AcquireFrames(NarrowDapInt(Request.threadId));
    for (int32 Index = 0; Index < Frames.Num(); ++Index)
    {
        const FBlueprintStackFrame& FrameInfo = Frames[Index];

        dap::StackFrame Frame;
        Frame.id = Index + 1;
        Frame.name = TCHAR_TO_UTF8(*FrameInfo.Name);
        Frame.line = FrameInfo.Line;
        Frame.column = 1;

        if (FrameInfo.SourceReference != 0)
        {
            const FString Path = FString::Printf(TEXT("blueprint-source:Blueprints/%s.cpp"), *FrameInfo.ClassName);

            dap::Source Source;
            Source.name = TCHAR_TO_UTF8(*FrameInfo.ClassName);
            Source.path = TCHAR_TO_UTF8(*Path);
            Source.sourceReference = FrameInfo.SourceReference;
            Frame.source = Source;
        }

        Response.stackFrames.push_back(Frame);
    }

    return Response;
}

dap::ResponseOrError<dap::ScopesResponse> FUBlueprintInspectorDap::ScopesRequest(const dap::ScopesRequest& Request)
{
    const FAcquireScopes AcquireScopes = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireScopes);
    if (!AcquireScopes)
    {
        return dap::Error("BlueprintInspector has no paused frame");
    }

    dap::ScopesResponse Response;
    for (const FBlueprintScope& ScopeInfo : AcquireScopes(NarrowDapInt(Request.frameId)))
    {
        dap::Scope Scope;
        Scope.name = TCHAR_TO_UTF8(*ScopeInfo.Name);
        Scope.variablesReference = ScopeInfo.VariablesReference;
        Scope.expensive = false;
        Response.scopes.push_back(Scope);
    }

    return Response;
}

dap::ResponseOrError<dap::VariablesResponse> FUBlueprintInspectorDap::VariablesRequest(const dap::VariablesRequest& Request)
{
    const FAcquireVariables AcquireVariables = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireVariables);
    if (!AcquireVariables)
    {
        return dap::Error("BlueprintInspector has no paused frame");
    }

    dap::VariablesResponse Response;
    for (const FBlueprintVariable& VariableInfo : AcquireVariables(NarrowDapInt(Request.variablesReference)))
    {
        dap::Variable Variable;
        Variable.name = TCHAR_TO_UTF8(*VariableInfo.Name);
        Variable.value = TCHAR_TO_UTF8(*VariableInfo.Value);
        if (!VariableInfo.Type.IsEmpty())
        {
            Variable.type = TCHAR_TO_UTF8(*VariableInfo.Type);
        }
        Variable.variablesReference = VariableInfo.VariablesReference;
        Response.variables.push_back(Variable);
    }

    return Response;
}

dap::ResponseOrError<dap::SetVariableResponse> FUBlueprintInspectorDap::SetVariableRequest(const dap::SetVariableRequest& Request)
{
    const FSetVariable SetVariable = GetCallback(&FBlueprintInspectorDapCallbacks::SetVariable);
    if (!SetVariable)
    {
        return dap::Error("BlueprintInspector has no paused frame");
    }

    const FBlueprintSetVariableResult Result = SetVariable(NarrowDapInt(Request.variablesReference), UTF8_TO_TCHAR(Request.name.c_str()), UTF8_TO_TCHAR(Request.value.c_str()));
    if (Result.bOutcomeUnknown)
    {
        return dap::Error(std::string(TCHAR_TO_UTF8(*Result.Error)));
    }
    if (!Result.bSuccess)
    {
        return dap::Error(std::string(TCHAR_TO_UTF8(*Result.Error)));
    }

    dap::SetVariableResponse Response;
    Response.value = TCHAR_TO_UTF8(*Result.Value);
    if (!Result.Type.IsEmpty())
    {
        Response.type = TCHAR_TO_UTF8(*Result.Type);
    }
    if (Result.VariablesReference != 0)
    {
        Response.variablesReference = Result.VariablesReference;
    }
    return Response;
}

dap::ResponseOrError<dap::EvaluateResponse> FUBlueprintInspectorDap::EvaluateRequest(const dap::EvaluateRequest& Request)
{
    const FEvaluateExpression Evaluate = GetCallback(&FBlueprintInspectorDapCallbacks::EvaluateExpression);
    if (!Evaluate)
    {
        return dap::Error("BlueprintInspector has no paused frame");
    }

    const int32 FrameId = Request.frameId.has_value() ? NarrowDapInt(Request.frameId.value()) : 1;
    const FBlueprintEvaluateResult Result = Evaluate(FrameId, UTF8_TO_TCHAR(Request.expression.c_str()));
    if (!Result.bSuccess)
    {
        return dap::Error(std::string(TCHAR_TO_UTF8(*Result.Error)));
    }

    dap::EvaluateResponse Response;
    Response.result = TCHAR_TO_UTF8(*Result.Result);
    if (!Result.Type.IsEmpty())
    {
        Response.type = TCHAR_TO_UTF8(*Result.Type);
    }
    Response.variablesReference = Result.VariablesReference;
    return Response;
}

dap::ResponseOrError<dap::PauseResponse> FUBlueprintInspectorDap::PauseRequest(const dap::PauseRequest&)
{
    return dap::Error("Pause is not implemented by BlueprintInspector");
}

dap::ResponseOrError<dap::ContinueResponse> FUBlueprintInspectorDap::ContinueRequest(const dap::ContinueRequest&)
{
    const FContinueExecution Continue = GetCallback(&FBlueprintInspectorDapCallbacks::ContinueExecution);
    if (Continue)
    {
        Continue();
    }
    return dap::ContinueResponse();
}

dap::ResponseOrError<dap::NextResponse> FUBlueprintInspectorDap::NextRequest(const dap::NextRequest&)
{
    const FStepExecution Step = GetCallback(&FBlueprintInspectorDapCallbacks::StepExecution);
    if (!Step)
    {
        return dap::Error("BlueprintInspector has no paused frame");
    }
    if (!Step(EBlueprintStepKind::Over))
    {
        return dap::Error("BlueprintInspector is not paused");
    }
    return dap::NextResponse();
}

dap::ResponseOrError<dap::StepInResponse> FUBlueprintInspectorDap::StepInRequest(const dap::StepInRequest&)
{
    const FStepExecution Step = GetCallback(&FBlueprintInspectorDapCallbacks::StepExecution);
    if (!Step)
    {
        return dap::Error("BlueprintInspector has no paused frame");
    }
    if (!Step(EBlueprintStepKind::Into))
    {
        return dap::Error("BlueprintInspector is not paused");
    }
    return dap::StepInResponse();
}

dap::ResponseOrError<dap::StepOutResponse> FUBlueprintInspectorDap::StepOutRequest(const dap::StepOutRequest&)
{
    const FStepExecution Step = GetCallback(&FBlueprintInspectorDapCallbacks::StepExecution);
    if (!Step)
    {
        return dap::Error("BlueprintInspector has no paused frame");
    }
    if (!Step(EBlueprintStepKind::Out))
    {
        return dap::Error("BlueprintInspector is not paused");
    }
    return dap::StepOutResponse();
}

dap::ResponseOrError<dap::SetBreakpointsResponse> FUBlueprintInspectorDap::SetBreakpointsRequest(const dap::SetBreakpointsRequest& Request)
{
    TArray<FBlueprintBreakpointRequest> Requests;
    TArray<int32> ResponseIndexByEntry;
    if (Request.breakpoints)
    {
        Requests.Reserve(static_cast<int32>(Request.breakpoints->size()));
        ResponseIndexByEntry.Init(INDEX_NONE, static_cast<int32>(Request.breakpoints->size()));
        int32 EntryIndex = 0;
        for (const dap::SourceBreakpoint& Requested : *Request.breakpoints)
        {
            // DAP carries the line as a 64-bit number; narrow it explicitly and reject values that cannot
            // name a rendered line instead of letting the cast wrap into a negative or unrelated line.
            // The rejected entry keeps its slot in the response: DAP matches breakpoints by index.
            if (Requested.line < 1 || Requested.line > static_cast<int64>(MAX_int32))
            {
                UE_LOG(LogBlueprintInspector, Warning, TEXT("setBreakpoints: ignoring requested line %lld outside the supported range"), static_cast<int64>(Requested.line));
                ++EntryIndex;
                continue;
            }

            FBlueprintBreakpointRequest Entry;
            Entry.Line = static_cast<int32>(Requested.line);
            if (Requested.condition.has_value())
            {
                Entry.Condition = UTF8_TO_TCHAR(Requested.condition->c_str());
            }
            if (Requested.hitCondition.has_value())
            {
                Entry.HitCondition = UTF8_TO_TCHAR(Requested.hitCondition->c_str());
            }
            if (Requested.logMessage.has_value())
            {
                Entry.LogMessage = UTF8_TO_TCHAR(Requested.logMessage->c_str());
            }

            ResponseIndexByEntry[EntryIndex] = Requests.Num();
            Requests.Add(MoveTemp(Entry));
            ++EntryIndex;
        }
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("setBreakpoints: %s, %d breakpoint(s) requested"), *DescribeSource(Request.source), Requests.Num());

    const FApplyBreakpoints ApplyBreakpoints = GetCallback(&FBlueprintInspectorDapCallbacks::ApplyBreakpoints);
    if (!ApplyBreakpoints)
    {
        return dap::Error("BlueprintInspector cannot set breakpoints");
    }

    const TGameThreadCallResult<TArray<FBlueprintBreakpointResolution>> BreakpointResult = ApplyBreakpoints(DescribeIdentity(Request.source), Requests);
    if (BreakpointResult.bTimedOut)
    {
        return dap::Error("The game thread did not service the setBreakpoints request");
    }
    const TArray<FBlueprintBreakpointResolution>& Resolutions = BreakpointResult.Value;

    const FIsPauseAvailable IsPauseAvailable = GetCallback(&FBlueprintInspectorDapCallbacks::IsPauseAvailable);
    // An unset callback means this caller has no pause path to report on; keep the previous behavior.
    const bool bPauseHooksReady = !IsPauseAvailable || IsPauseAvailable();

    dap::SetBreakpointsResponse Response;
    if (Request.breakpoints)
    {
        int32 EntryIndex = 0;
        for (const dap::SourceBreakpoint& Requested : *Request.breakpoints)
        {
            dap::Breakpoint Breakpoint;
            Breakpoint.source = Request.source;
            Breakpoint.line = static_cast<dap::integer>(Requested.line);

            const int32 ResponseIndex = ResponseIndexByEntry.IsValidIndex(EntryIndex) ? ResponseIndexByEntry[EntryIndex] : INDEX_NONE;
            if (ResponseIndex != INDEX_NONE && Resolutions.IsValidIndex(ResponseIndex) && Resolutions[ResponseIndex].Line > 0)
            {
                Breakpoint.verified = true;
                Breakpoint.line = Resolutions[ResponseIndex].Line;
                if (!bPauseHooksReady)
                {
                    // Keep the breakpoint so the client does not drop it while the UWorld hooks are down,
                    // but say plainly that it cannot hit until they attach.
                    Breakpoint.message = "Pause hooks are not attached; this breakpoint will not hit until they are. Check the editor log.";
                }
            }
            else
            {
                Breakpoint.verified = false;
                if (ResponseIndex == INDEX_NONE)
                {
                    Breakpoint.message = "The requested line is outside the supported range";
                }
                else if (Resolutions.IsValidIndex(ResponseIndex) && !Resolutions[ResponseIndex].bSourceAvailable)
                {
                    Breakpoint.message = "The source is not available yet";
                }
                else
                {
                    Breakpoint.message = "No breakpointable line at or below this one";
                }
            }

            Response.breakpoints.push_back(Breakpoint);
            ++EntryIndex;
        }
    }

    return Response;
}

dap::ResponseOrError<dap::SourceResponse> FUBlueprintInspectorDap::SourceRequest(const dap::SourceRequest& Request)
{
    dap::SourceResponse Response;
    const FAcquireClassFunctions AcquireClassFunctions = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireClassFunctions);
    if (!AcquireClassFunctions)
    {
        // An empty successful response would render as blank pseudo-source; the client cannot tell that
        // apart from a genuinely empty function.
        return dap::Error("BlueprintInspector cannot serve source");
    }

    if (Request.sourceReference < 0 || Request.sourceReference > static_cast<int64>(MAX_int32))
    {
        return dap::Error("Invalid sourceReference");
    }
    const TGameThreadCallResult<FString> SourceResult = AcquireClassFunctions(NarrowDapInt(Request.sourceReference));
    if (SourceResult.bTimedOut)
    {
        return dap::Error("The game thread did not service the source request");
    }
    const FString& SourceCode = SourceResult.Value;
    FString SourcePath = TEXT("<no path>");
    if (Request.source.has_value() && Request.source.value().path.has_value())
    {
        SourcePath = UTF8_TO_TCHAR(Request.source.value().path.value().c_str());
    }
    UE_LOG(LogBlueprintInspector, Log, TEXT("source %d (%s): %d character(s)"), NarrowDapInt(Request.sourceReference), *SourcePath, SourceCode.Len());
    Response.content = TCHAR_TO_UTF8(*SourceCode);
    return Response;
}

dap::ResponseOrError<dap::DisconnectResponse> FUBlueprintInspectorDap::DisconnectRequest(const dap::DisconnectRequest&, const uint64 Generation)
{
    // Disconnect is intentionally not registered through the gate, so the client can always tear the
    // connection down; the side effect that resumes the parked game thread still needs this connection
    // (not whichever one is current) to be authenticated.
    if (Auth.IsAuthenticatedFor(Generation))
    {
        const FContinueExecution Continue = GetCallback(&FBlueprintInspectorDapCallbacks::ContinueExecution);
        if (Continue)
        {
            Continue();
        }
    }

    dap::DisconnectResponse Response;
    return Response;
}

dap::ResponseOrError<dap::ConfigurationDoneResponse> FUBlueprintInspectorDap::ConfigurationDoneRequest(const dap::ConfigurationDoneRequest&)
{
    dap::ConfigurationDoneResponse Response;
    return Response;
}

dap::ResponseOrError<dap::BlueprintCaptureState> FUBlueprintInspectorDap::StartCaptureRequest(const dap::BlueprintStartCaptureRequest&)
{
    const FCaptureControl Start = GetCallback(&FBlueprintInspectorDapCallbacks::StartCapture);
    return ToCaptureState(Start ? Start() : FBlueprintHotCaptureStatus());
}

dap::ResponseOrError<dap::BlueprintCaptureState> FUBlueprintInspectorDap::StopCaptureRequest(const dap::BlueprintStopCaptureRequest&)
{
    const FCaptureControl Stop = GetCallback(&FBlueprintInspectorDapCallbacks::StopCapture);
    return ToCaptureState(Stop ? Stop() : FBlueprintHotCaptureStatus());
}

dap::ResponseOrError<dap::BlueprintCaptureState> FUBlueprintInspectorDap::ResetCaptureRequest(const dap::BlueprintResetCaptureRequest&)
{
    const FCaptureControl Reset = GetCallback(&FBlueprintInspectorDapCallbacks::ResetCapture);
    return ToCaptureState(Reset ? Reset() : FBlueprintHotCaptureStatus());
}

dap::ResponseOrError<dap::BlueprintCaptureState> FUBlueprintInspectorDap::CaptureStateRequest(const dap::BlueprintCaptureStateRequest&)
{
    const FCaptureControl Query = GetCallback(&FBlueprintInspectorDapCallbacks::QueryCapture);
    return ToCaptureState(Query ? Query() : FBlueprintHotCaptureStatus());
}

dap::ResponseOrError<dap::BlueprintHotLinesResponse> FUBlueprintInspectorDap::HotLinesRequest(const dap::BlueprintHotLinesRequest& Request)
{
    dap::BlueprintHotLinesResponse Response;

    const FAcquireHotLines Acquire = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireHotLines);
    if (!Acquire)
    {
        return dap::Error("The hot-line callback is not installed");
    }
    if (Request.maxLines < 0 || Request.maxLines > static_cast<int64>(MAX_int32))
    {
        return dap::Error("Invalid maxLines");
    }
    const TGameThreadCallResult<FBlueprintHotLinesResult> HotResult = Acquire(NarrowDapInt(Request.maxLines));
    if (HotResult.bTimedOut)
    {
        return dap::Error("The game thread did not service the hot-line request");
    }
    if (HotResult.bUnavailable)
    {
        return dap::Error("The game thread is paused; the hot-line request is not available until it resumes");
    }
    const FBlueprintHotLinesResult& Result = HotResult.Value;

    Response.state = ToCaptureState(Result.Status);

    const FAcquireCaptureMetadata AcquireMetadata = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireCaptureMetadata);
    FBlueprintCaptureMetadata Metadata;
    if (AcquireMetadata)
    {
        const TGameThreadCallResult<FBlueprintCaptureMetadata> MetadataResult = AcquireMetadata();
        if (MetadataResult.bTimedOut)
        {
            return dap::Error("The game thread did not service the capture metadata request");
        }
        if (MetadataResult.bUnavailable)
        {
            return dap::Error("The game thread is paused; the capture metadata is not available until it resumes");
        }
        Metadata = MetadataResult.Value;
    }
    Response.metadata.map = TCHAR_TO_UTF8(*Metadata.Map);
    Response.metadata.build = TCHAR_TO_UTF8(*Metadata.Build);
    Response.metadata.engine = TCHAR_TO_UTF8(*Metadata.Engine);
    Response.metadata.project = TCHAR_TO_UTF8(*Metadata.Project);
    Response.metadata.capturedAt = TCHAR_TO_UTF8(*Metadata.CapturedAt);

    Response.lines.reserve(Result.Lines.Num());
    for (const FBlueprintHotLine& Line : Result.Lines)
    {
        dap::BlueprintHotLine Entry;
        Entry.className = TCHAR_TO_UTF8(*Line.ClassName);
        Entry.functionName = TCHAR_TO_UTF8(*Line.FunctionName);
        Entry.line = Line.Line;
        Entry.hits = Line.Hits;
        Entry.kind = Line.bTransfer ? "transfer" : "statement";
        Response.lines.push_back(Entry);
    }
    return Response;
}

dap::ResponseOrError<dap::BlueprintCoverageResponse> FUBlueprintInspectorDap::CoverageRequest(const dap::BlueprintCoverageRequest& Request)
{
    dap::BlueprintCoverageResponse Response;

    const FAcquireCoverage Acquire = GetCallback(&FBlueprintInspectorDapCallbacks::AcquireCoverage);
    if (!Acquire)
    {
        return dap::Error("The coverage callback is not installed");
    }
    if (Request.maxLines < 0 || Request.maxLines > static_cast<int64>(MAX_int32))
    {
        return dap::Error("Invalid maxLines");
    }
    const TGameThreadCallResult<FBlueprintCoverageResult> CoverageResult = Acquire(NarrowDapInt(Request.maxLines));
    if (CoverageResult.bTimedOut)
    {
        return dap::Error("The game thread did not service the coverage request");
    }
    if (CoverageResult.bUnavailable)
    {
        return dap::Error("The game thread is paused; the coverage request is not available until it resumes");
    }
    const FBlueprintCoverageResult& Result = CoverageResult.Value;

    Response.state = ToCaptureState(Result.Status);
    Response.renderedClasses = Result.RenderedClasses;
    Response.totalLines = Result.TotalLines;
    Response.coveredLines = Result.CoveredLines;
    Response.uncoveredLines = Result.UncoveredLines;
    Response.truncated = Result.bTruncated;
    Response.lines.reserve(Result.Lines.Num());
    for (const FBlueprintCoverageLine& Line : Result.Lines)
    {
        dap::BlueprintCoverageLine Entry;
        Entry.className = TCHAR_TO_UTF8(*Line.ClassName);
        Entry.functionName = TCHAR_TO_UTF8(*Line.FunctionName);
        Entry.line = Line.Line;
        Entry.hits = Line.Hits;
        Entry.kind = Line.bTransfer ? "transfer" : "statement";
        Response.lines.push_back(MoveTemp(Entry));
    }
    return Response;
}

dap::ResponseOrError<dap::SetExceptionBreakpointsResponse> FUBlueprintInspectorDap::SetExceptionBreakpointsRequest(const dap::SetExceptionBreakpointsRequest& Request)
{
    // Replace-the-whole-set semantics, like setBreakpoints: whatever arrives is the complete selection.
    TArray<FString> Filters;
    Filters.Reserve(static_cast<int32>(Request.filters.size()));
    for (const dap::string& Filter : Request.filters)
    {
        Filters.Add(FString(UTF8_TO_TCHAR(Filter.c_str())));
    }

    const FApplyExceptionFilters Apply = GetCallback(&FBlueprintInspectorDapCallbacks::ApplyExceptionFilters);
    if (Apply)
    {
        Apply(Filters);
    }

    UE_LOG(LogBlueprintInspector, Log, TEXT("Exception filters set: %d"), Filters.Num());
    return dap::SetExceptionBreakpointsResponse();
}

dap::ResponseOrError<dap::BlueprintAuditResponse> FUBlueprintInspectorDap::AuditRequest(const dap::BlueprintAuditRequest& Request)
{
    dap::BlueprintAuditResponse Response;

    const FRunAudit Run = GetCallback(&FBlueprintInspectorDapCallbacks::RunAudit);
    if (!Run)
    {
        return dap::Error("The audit callback is not installed");
    }

    const TGameThreadCallResult<FBlueprintAuditResult> AuditResult = Run(NarrowDapInt(Request.maxFindings));
    if (AuditResult.bTimedOut)
    {
        return dap::Error("The game thread did not service the audit request");
    }
    if (AuditResult.bUnavailable)
    {
        return dap::Error("The game thread is paused; the audit is not available until it resumes");
    }
    const FBlueprintAuditResult& Result = AuditResult.Value;

    Response.classes = Result.ClassesScanned;
    Response.functions = Result.FunctionsScanned;
    Response.truncated = Result.bTruncated;
    Response.findings.reserve(Result.Findings.Num());
    for (const FBlueprintAuditFinding& Finding : Result.Findings)
    {
        dap::BlueprintAuditFinding Entry;
        Entry.rule = TCHAR_TO_UTF8(*Finding.Rule);
        Entry.severity = TCHAR_TO_UTF8(BlueprintAudit::SeverityToString(Finding.Severity));
        Entry.category = TCHAR_TO_UTF8(*Finding.Category);
        Entry.className = TCHAR_TO_UTF8(*Finding.ClassName);
        Entry.functionName = TCHAR_TO_UTF8(*Finding.FunctionName);
        Entry.line = Finding.Line;
        Entry.message = TCHAR_TO_UTF8(*Finding.Message);
        Entry.evidence = TCHAR_TO_UTF8(*Finding.Evidence.ToString());
        Response.findings.push_back(MoveTemp(Entry));
    }
    return Response;
}