// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"

#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR

#include "BlueprintInspector.h"
#include "BlueprintInspectorDAP.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"

#include "dap/network.h"
#include "dap/session.h"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <thread>

// Shared harness for the DAP tests. Each test owns one FDapTestWorker for its lifetime; the worker
// polls the stop flag between requests, so a test that times out can stop and join it instead of
// detaching a thread that would keep driving the adapter through PIE teardown.
//
// The contract for a client body:
//  * every wait (connect retry, token file, game-thread flag, request future) goes through a loop that
//    gives up when Stop() is set, so the body returns on its own;
//  * cleanup requests that release the parked game thread (continue, clear breakpoints) pass
//    bIgnoreStop = true, because aborting them would leave the VM parked;
//  * the body's completion flag is raised by FDoneGuard, and the guard is declared after the session so
//    the flag is set before the socket closes.
namespace BlueprintDapTest
{
// The parts of the client state every DAP test shares. A test derives from this and adds its own
// atomics; Fail keeps the first message so a later cascade cannot hide the cause.
struct FClientState
{
    std::atomic<bool> bDone{false};

    FCriticalSection ErrorLock;
    FString Error;

    void Fail(const FString& Message)
    {
        FScopeLock Lock(&ErrorLock);
        if (Error.IsEmpty())
        {
            Error = Message;
        }
    }

    FString GetError()
    {
        FScopeLock Lock(&ErrorLock);
        return Error;
    }
};

// Raises the completion flag from any exit path. Declared after the session, it runs before the socket
// closes, so the closed handler does not report a normal shutdown as a failure.
struct FDoneGuard
{
    std::atomic<bool>* Done;

    explicit FDoneGuard(std::atomic<bool>& InDone) : Done(&InDone)
    {
    }

    FDoneGuard(const FDoneGuard&) = delete;
    FDoneGuard& operator=(const FDoneGuard&) = delete;

    ~FDoneGuard()
    {
        Done->store(true);
    }
};

// The polling primitive behind every client wait: returns true when the predicate holds, false when the
// client was asked to stop or the deadline passed. Sleeping in slices keeps a stop request from waiting
// out a long timeout.
template <typename PredicateType>
bool WaitWhileRunning(const std::atomic<bool>& Stop, double Deadline, PredicateType&& Predicate, float SliceSeconds = 0.01f)
{
    for (;;)
    {
        if (Predicate())
        {
            return true;
        }
        if (Stop.load())
        {
            return false;
        }

        const double Remaining = Deadline - FPlatformTime::Seconds();
        if (Remaining <= 0.0)
        {
            return false;
        }
        FPlatformProcess::Sleep(FMath::Min(SliceSeconds, static_cast<float>(Remaining)));
    }
}

// Sends one request and waits for its response in stop-aware slices. bIgnoreStop is for the cleanup
// requests a client must still answer while the game thread is parked;
// without it, an abandoned test would leave the VM parked forever.
template <typename StateType, typename RequestType>
bool Send(dap::Session& Session, const RequestType& Request, typename RequestType::Response& OutResponse, StateType& State, const std::atomic<bool>& Stop, const TCHAR* Name, double TimeoutSeconds = 5.0, bool bIgnoreStop = false)
{
    if (!bIgnoreStop && Stop.load())
    {
        State.Fail(FString::Printf(TEXT("%s was abandoned because the test stopped its client."), Name));
        return false;
    }

    auto Future = Session.send(Request);
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    while (Future.wait_for(std::chrono::milliseconds(50)) != dap::future_status::ready)
    {
        if (!bIgnoreStop && Stop.load())
        {
            State.Fail(FString::Printf(TEXT("%s was abandoned because the test stopped its client."), Name));
            return false;
        }
        if (FPlatformTime::Seconds() >= Deadline)
        {
            State.Fail(bIgnoreStop ? FString::Printf(TEXT("%s did not complete; the parked game thread may not be released."), Name) : FString::Printf(TEXT("%s timed out."), Name));
            return false;
        }
    }

    auto Result = Future.get();
    if (Result.error)
    {
        State.Fail(FString::Printf(TEXT("%s failed: %s."), Name, UTF8_TO_TCHAR(Result.error.message.c_str())));
        return false;
    }

    OutResponse = MoveTemp(Result.response);
    return true;
}

// Waits for the session token the listener publishes, or gives up when the client is stopped.
inline bool ReadSessionToken(const FString& TokenPath, const std::atomic<bool>& Stop, FString& OutToken, FClientState& State, double TimeoutSeconds = 5.0)
{
    const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
    const bool bLoaded = WaitWhileRunning(
        Stop, Deadline,
        [&TokenPath, &OutToken]() {
            // Read into a local so a stale OutToken or a partially written file cannot pass as loaded.
            FString Read;
            if (!FFileHelper::LoadFileToString(Read, *TokenPath))
            {
                return false;
            }
            Read.TrimStartAndEndInline();
            if (Read.Len() != 32)
            {
                return false;
            }
            for (const TCHAR Character : Read)
            {
                if (!FChar::IsHexDigit(Character))
                {
                    return false;
                }
            }
            OutToken = MoveTemp(Read);
            return true;
        },
        0.05f);

    if (!bLoaded || OutToken.IsEmpty())
    {
        if (!Stop.load())
        {
            State.Fail(TEXT("The DAP session token was not written."));
        }
        return false;
    }
    return true;
}

// Connects to the listener the subsystem started for this run. The retry loop is bounded and stops
// early when the test gives up.
inline std::shared_ptr<dap::ReaderWriter> Connect(const std::atomic<bool>& Stop, FClientState& State)
{
    std::shared_ptr<dap::ReaderWriter> ReaderWriter;
    const double ConnectDeadline = FPlatformTime::Seconds() + 5.0;
    for (int32 Attempt = 0; Attempt < 50 && !ReaderWriter && !Stop.load() && FPlatformTime::Seconds() < ConnectDeadline; ++Attempt)
    {
        const double Remaining = ConnectDeadline - FPlatformTime::Seconds();
        if (Remaining <= 0.0)
        {
            break;
        }
        const int32 TimeoutMs = FMath::Clamp(static_cast<int32>(Remaining * 1000.0), 1, 250);
        ReaderWriter = dap::net::connect("127.0.0.1", FUBlueprintInspectorDap::ListeningPort(), TimeoutMs);
        if (!ReaderWriter)
        {
            FPlatformProcess::Sleep(0.1f);
        }
    }

    if (!ReaderWriter && !Stop.load())
    {
        State.Fail(FString::Printf(TEXT("Could not connect to the DAP listener on 127.0.0.1:%d."), FUBlueprintInspectorDap::ListeningPort()));
    }
    return ReaderWriter;
}

// Owns the client thread. Start once, then request a stop. The destructor stops the client and joins a
// finished thread; a client that has not finished is detached with its shared state and keep-alive.
class FWorker
{
public:
    FWorker() = default;
    ~FWorker()
    {
        RequestStop();
        if (Worker.joinable() && !JoinIfFinished(0.0))
        {
            // The thread owns the shared state and the keep-alive passed to Start, so detaching it
            // cannot leave those objects freed under the body.
            Worker.detach();
        }
    }

    FWorker(const FWorker&) = delete;
    FWorker& operator=(const FWorker&) = delete;

    // Returns false when the thread could not be started. Exceptions escaping the body are recorded on
    // State instead of terminating the editor. State and KeepAlive are captured by the thread, so a
    // worker that outlives its fixture keeps them alive.
    bool Start(TSharedPtr<FClientState> State, TSharedPtr<const void> KeepAlive, TFunction<void()> Body)
    {
        if (!State.IsValid())
        {
            return false;
        }
        if (Signal->bRunning.load())
        {
            State->Fail(TEXT("A DAP test worker was started twice."));
            return false;
        }
        if (Worker.joinable() && !JoinIfFinished(0.0))
        {
            State->Fail(TEXT("A previous DAP test worker could not be joined before a restart."));
            return false;
        }
        Signal->bStop.store(false);
        Signal->bFinished.store(false);
        Signal->bRunning.store(true);
        try
        {
            Worker = std::thread([Signal = Signal, State, KeepAlive = MoveTemp(KeepAlive), Body = MoveTemp(Body)]() {
                try
                {
                    Body();
                }
                catch (const std::exception& Exception)
                {
                    // cppdap throws on a closed writer or a protocol error; escaping the thread would
                    // terminate the editor instead of failing the test.
                    State->Fail(FString::Printf(TEXT("The DAP client threw: %s"), UTF8_TO_TCHAR(Exception.what())));
                }
                catch (...)
                {
                    State->Fail(TEXT("The DAP client threw an unknown exception."));
                }
                Signal->bRunning.store(false);
                Signal->bFinished.store(true);
            });
        }
        catch (const std::exception& Exception)
        {
            Signal->bRunning.store(false);
            State->Fail(FString::Printf(TEXT("The DAP client thread could not start: %s"), UTF8_TO_TCHAR(Exception.what())));
            return false;
        }
        return true;
    }

    bool IsStarted() const
    {
        return Signal->bRunning.load() || Worker.joinable();
    }

    // The flag every client wait polls.
    const std::atomic<bool>& Stop() const
    {
        return Signal->bStop;
    }

    void RequestStop()
    {
        Signal->bStop.store(true);
    }

    // Waits, without joining, until the client body has returned.
    bool WaitForExit(double TimeoutSeconds)
    {
        if (Worker.get_id() == std::this_thread::get_id())
        {
            return false;
        }
        if (!Worker.joinable() && !Signal->bFinished.load())
        {
            return false;
        }
        const double Deadline = FPlatformTime::Seconds() + TimeoutSeconds;
        while (!Signal->bFinished.load())
        {
            if (FPlatformTime::Seconds() >= Deadline)
            {
                return false;
            }
            FPlatformProcess::Sleep(0.01f);
        }
        return true;
    }

    // The timeout path: stop the client, then join it. Returns false when it did not stop in time; the
    // thread stays joinable so Shutdown() can finish the job rather than silently detaching.
    bool StopAndJoin(double TimeoutSeconds)
    {
        RequestStop();
        return JoinAfterExit(TimeoutSeconds);
    }

    // Joins the client once its body has returned. The completion flag the commands observe is raised
    // at the end of the body, so this waits a short, bounded time for the thread to leave its last
    // frame rather than letting it outlive the command into PIE teardown.
    bool JoinIfFinished(double TimeoutSeconds = 5.0)
    {
        if (!Worker.joinable())
        {
            return false;
        }
        if (!Signal->bFinished.load() && !WaitForExit(TimeoutSeconds))
        {
            return false;
        }
        return Join();
    }

    // Stops and joins the client, waiting up to TimeoutSeconds. Safe to call more than once.
    bool Shutdown(double TimeoutSeconds = 10.0)
    {
        if (!Worker.joinable())
        {
            return true;
        }
        return StopAndJoin(TimeoutSeconds);
    }

private:
    struct FSignal
    {
        std::atomic<bool> bStop{false};
        std::atomic<bool> bFinished{false};
        std::atomic<bool> bRunning{false};
    };

    bool Join()
    {
        if (Worker.get_id() == std::this_thread::get_id())
        {
            // The fixture was released from its own client thread; joining it here would deadlock.
            return false;
        }

        try
        {
            Worker.join();
            return true;
        }
        catch (const std::exception& Exception)
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("A DAP test client could not be joined: %s"), UTF8_TO_TCHAR(Exception.what()));
            return false;
        }
    }

    bool JoinAfterExit(double TimeoutSeconds)
    {
        if (!Worker.joinable())
        {
            return true;
        }
        if (Worker.get_id() == std::this_thread::get_id())
        {
            return false;
        }
        if (!WaitForExit(TimeoutSeconds))
        {
            UE_LOG(LogBlueprintInspector, Error, TEXT("A DAP test client ignored its stop request for %.1f s; the worker was left running so the fixture can report it."), TimeoutSeconds);
            return false;
        }
        return Join();
    }

    std::shared_ptr<FSignal> Signal = std::make_shared<FSignal>();
    std::thread Worker;
};
} // namespace BlueprintDapTest

#endif // WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
