#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"
// FScopeLock is used inline here; 4.27 does not reach it transitively through CoreMinimal.
#include "Misc/ScopeLock.h"
#include "UObject/Script.h"

#include <cstddef>

class FField;

struct FBlueprintOpcodeStats
{
    int32 Opcodes = 0;
    int32 Unsupported = 0;
};

class FBlueprintOpcodeLogger
{
public:
    using FOpcodeSink = TFunction<void(EExprToken, int64)>;
    // The stats snapshot is passed by value: the sink may keep it, and a reference would dangle once
    // LogSummary returns.
    using FSummarySink = TFunction<void(FString FunctionName, FBlueprintOpcodeStats)>;

    void BeginRun();
    void LogSummary(const FString& FunctionName) const;
    void CountUnsupported();

    void LogOpcode(EExprToken InOpcode, int64 InScriptIndex);

    // Optional capture hook for tests and tools. The sink runs for every opcode decoded while it is
    // installed, independently of the bp.LogOpcodes console variable; install it before decoding starts.
    // It runs on whichever thread decodes and outside the sink lock, so it may be called concurrently
    // and must be thread-safe itself.
    static void SetOpcodeSink(FOpcodeSink InSink);
    static void ClearOpcodeSink();
    static void SetSummarySink(FSummarySink InSink);
    static void ClearSummarySink();
    static FString OpcodeToString(EExprToken InOpcode);

    void LogValue(const TCHAR* InValue);
    void LogValue(const FString& InValue);
    void LogValue(const FScriptName& InValue);
    void LogValue(int32 InValue);
    void LogValue(int64 InValue);
    void LogValue(uint32 InValue);
    void LogValue(uint64 InValue);
    void LogValue(double InValue);
    void LogValue(bool InValue);
    void LogValue(std::nullptr_t);
    void LogValue(const UObject* InValue);
    void LogValue(const FField* InValue);

    void Indent()
    {
        FScopeLock Lock(&StatsLock);
        // Keep the nesting counter balanced with Unindent; only the rendered indent is clamped.
        ++IndentLevel;
        UpdateIndentString();
    }

    void Unindent()
    {
        FScopeLock Lock(&StatsLock);
        if (IndentLevel == 0)
        {
            return;
        }
        --IndentLevel;
        UpdateIndentString();
    }

private:
    static const TCHAR* GetOpcodeName(EExprToken InOpcode);
    static bool IsEnabled();

    static FCriticalSection SinkLock;
    static FOpcodeSink Sink;
    static TAtomic<bool> bSinkSet;
    static FCriticalSection SummarySinkLock;
    static FSummarySink SummarySink;
    // Every opcode increments Stats and the summary reads it, so the counters are guarded even though a
    // logger normally belongs to one decoding thread. StatsLock also guards IndentLevel.
    mutable FCriticalSection StatsLock;
    FBlueprintOpcodeStats Stats;

    void UpdateIndentString()
    {
        const size_t RenderedLevel = FMath::Min(IndentLevel, MaxIndentLevel);
        if (RenderedLevel == RenderedIndentLevel)
        {
            return;
        }
        RenderedIndentLevel = RenderedLevel;
        IndentString = RenderedLevel == 0
                           ? FString()
                           : FString::ChrN(static_cast<int32>(RenderedLevel * AdjustAmount - 1), TEXT(' ')) + TEXT("\u25b6");
    }

    // By value, not a borrowed pointer: the buffer is reallocated whenever the indent changes, and the
    // lock this takes is released before the caller formats the log line.
    FString GetIndentString() const
    {
        FScopeLock Lock(&StatsLock);
        return IndentString;
    }

    static constexpr int32 AdjustAmount = 5;
    static constexpr size_t MaxIndentLevel = 7;
    FString IndentString;
    size_t RenderedIndentLevel = static_cast<size_t>(-1);
    size_t IndentLevel = 0;
};
