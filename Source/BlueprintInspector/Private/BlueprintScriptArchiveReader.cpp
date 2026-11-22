#include "BlueprintScriptArchiveReader.h"
#include "BlueprintInspector.h"

void FBlueprintScriptArchiveReader::OnReadError(const int64 InOffset, const int64 InLength)
{
    UE_LOG(LogBlueprintInspector, Error, TEXT("FBlueprintScriptArchiveReader out of bounds: Offset=%lld Length=%lld Script=%d"), InOffset, InLength, Script.Num());
    SetError();
}

FArchive& FBlueprintScriptArchiveReader::operator<<(FField*& Pointer)
{
    // The bytecode stores an object reference as one pointer-width slot, so what is copied is the pointer
    // itself: Serialize takes a byte count, and the width of the object it points at would be meaningless.
    Pointer = nullptr;
    ReadRaw(static_cast<void*>(&Pointer), sizeof(FField*));
    return *this;
}

FArchive& FBlueprintScriptArchiveReader::operator<<(UObject*& Pointer)
{
    Pointer = nullptr;
    ReadRaw(static_cast<void*>(&Pointer), sizeof(UObject*));
    return *this;
}

FArchive& FBlueprintScriptArchiveReader::operator<<(FRotator& Rotator)
{
    FMemory::Memzero(&Rotator, sizeof(FRotator));
    ReadRaw(&Rotator, sizeof(FRotator));
    return *this;
}

FArchive& FBlueprintScriptArchiveReader::operator<<(FScriptName& ScriptName)
{
    FMemory::Memzero(&ScriptName, sizeof(FScriptName));
    ReadRaw(&ScriptName, sizeof(FScriptName));
    return *this;
}

FArchive& FBlueprintScriptArchiveReader::operator<<(FString& OutString)
{
    CheckScriptStorage();

    OutString.Reset();
    if (IsError() || Offset < 0 || Offset >= Script.Num())
    {
        SetError();
        return *this;
    }

    const EExprToken Opcode = static_cast<EExprToken>(Script[static_cast<int32>(Offset++)]);
    switch (Opcode)
    {
    case EX_StringConst:
    {
        const int64 Remaining = Script.Num() - Tell();
        const ANSICHAR* const Ptr = GetCurrentScriptPointer<ANSICHAR>();
        if (Ptr == nullptr)
        {
            OnReadError(Tell(), Remaining);
            break;
        }
        int64 Count = 0;
        while (Count < Remaining && Ptr[Count] != '\0')
        {
            ++Count;
        }
        if (Count == Remaining)
        {
            // An unterminated literal is a truncated operand, not a recoverable read.
            OnReadError(Tell(), Remaining);
            break;
        }
        OutString = FString(StringCast<TCHAR>(Ptr).Get());
        Seek(Tell() + Count + 1);
    }
    break;

    case EX_UnicodeStringConst:
    {
        const int64 RemainingBytes = Script.Num() - Tell();
        const int64 RemainingWide = RemainingBytes / static_cast<int64>(sizeof(WIDECHAR));
        const WIDECHAR* const Ptr = GetCurrentScriptPointer<WIDECHAR>();
        if (Ptr == nullptr)
        {
            OnReadError(Tell(), RemainingBytes);
            break;
        }

        // The operand is not aligned, so read through bytes and materialise an aligned copy. Find the
        // terminator first: sizing the buffer from the remaining script would allocate up to the whole
        // tail for every short literal.
        int64 Count = 0;
        WIDECHAR Char = 0;
        while (Count < RemainingWide)
        {
            FMemory::Memcpy(&Char, reinterpret_cast<const uint8*>(Ptr) + Count * sizeof(WIDECHAR), sizeof(WIDECHAR));
            if (Char == TEXT('\0'))
            {
                break;
            }
            ++Count;
        }
        if (Count == RemainingWide)
        {
            OnReadError(Tell(), RemainingBytes);
            break;
        }

        TArray<WIDECHAR> Buffer;
        Buffer.SetNumZeroed(static_cast<int32>(Count) + 1);
        if (Count > 0)
        {
            FMemory::Memcpy(Buffer.GetData(), reinterpret_cast<const uint8*>(Ptr), Count * static_cast<int64>(sizeof(WIDECHAR)));
        }
        OutString = Buffer.GetData();
        Seek(Tell() + (Count + 1) * static_cast<int64>(sizeof(WIDECHAR)));
    }
    break;

    default:
        UE_LOG(LogBlueprintInspector, Error, TEXT("FBlueprintScriptArchiveReader::operator<<(FString&) - Unexpected opcode. Expected %d or %d, got %d"), EX_StringConst, EX_UnicodeStringConst, Opcode);
        SetError();
        break;
    }

    return *this;
}

void FBlueprintScriptArchiveReader::Serialize(void* V, const int64 Length)
{
    // Geometry constants and other Serialize() payloads need a deterministic value even when the script
    // ended mid-operand, and even when the archive is already in error: zero first, then read.
    if (V != nullptr && Length > 0)
    {
        FMemory::Memzero(V, Length);
    }

    // Once a read has failed the cursor may sit at a clamped offset; short-circuit so the archive does
    // not keep handing out plausible-looking operands after the failure.
    if (IsError())
    {
        return;
    }

    ReadRaw(V, Length);
}

void FBlueprintScriptArchiveReader::Seek(const int64 InOffset)
{
    if (InOffset < 0 || InOffset > Script.Num())
    {
        UE_LOG(LogBlueprintInspector, Error, TEXT("FBlueprintScriptArchiveReader::Seek out of bounds: Offset=%lld Script=%d"), InOffset, Script.Num());
        SetError();
        Offset = FMath::Clamp<int64>(InOffset, 0, Script.Num());
        return;
    }
    Offset = InOffset;
}

int64 FBlueprintScriptArchiveReader::Tell()
{
    return Offset;
}

int64 FBlueprintScriptArchiveReader::Length() const
{
    return Script.Num();
}

int64 FBlueprintScriptArchiveReader::TotalSize()
{
    return Script.Num();
}
