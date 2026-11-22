#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Math/Rotator.h"
#include "Templates/UnrealTemplate.h"

#include "HAL/UnrealMemory.h"
#include "Misc/AssertionMacros.h"
#include "Serialization/Archive.h"
#include "UObject/Field.h"
#include "UObject/Script.h"
#include <type_traits>

class FBlueprintScriptArchiveReader final : public FArchive
{
public:
    static const TArray<uint8>& GetEmptyScript()
    {
        static const TArray<uint8> Empty;
        return Empty;
    }

    // The reader borrows the script: InScript must outlive it, and must not be reset or reallocated
    // while the reader is alive. An rvalue is rejected so a temporary cannot bind; pass a named array
    // (the disassembler points at UFunction::Script, which lives for the whole pass).
    explicit FBlueprintScriptArchiveReader(const TArray<uint8>& InScript = GetEmptyScript()) : Script(InScript), ScriptStorage(InScript.GetData())
    {
        FArchive::SetIsLoading(true);
        FArchive::SetIsPersistent(true);
    }

    FBlueprintScriptArchiveReader(TArray<uint8>&& InScript) = delete;

    // Reinitializes the script state by constructing the reader again in place.
    void Rebind(const TArray<uint8>& InScript)
    {
        this->~FBlueprintScriptArchiveReader();
        new (this) FBlueprintScriptArchiveReader(InScript);
    }

    void Rebind(TArray<uint8>&& InScript) = delete;

    // Expose the base-class overloads, which our own overloads would otherwise hide. This must stay in
    // a public section: a using-declaration inherits the access of the section it appears in, so placing
    // it under private: would make `Reader << SomeFName` ill-formed instead of using FArchive's reader.
    using FArchive::operator<<;

    FArchive& operator<<(FField*& Pointer) override;
    FArchive& operator<<(UObject*& Pointer) override;

    FORCEINLINE bool AtEnd() override
    {
        return Offset >= Script.Num();
    }

    FORCEINLINE bool ReadByte(uint8& Out)
    {
        CheckScriptStorage();

        // Once the archive is in error, keep returning a reproducible zero instead of handing out
        // further operands from the clamped cursor (Serialize() already behaves this way).
        Out = 0;
        if (IsError())
        {
            return false;
        }
        if (Offset < 0 || Offset >= Script.Num())
        {
            OnReadError(Offset, 1);
            return false;
        }

        Out = Script[static_cast<int32>(Offset++)];
        return true;
    }

    FORCEINLINE bool ReadRaw(void* Out, const int64 Length)
    {
        CheckScriptStorage();
        if (IsError())
        {
            return false;
        }

        // Compare against the remaining bytes rather than `Offset + Length`: the sum can overflow and
        // turn a huge Length into a value that passes the check.
        if (Length < 0 || Offset < 0 || Offset > Script.Num() || Length > static_cast<int64>(Script.Num()) - Offset || (Out == nullptr && Length > 0))
        {
            OnReadError(Offset, Length);
            return false;
        }

        if (Length == 0)
        {
            // memcpy requires non-null pointers even for zero bytes, and Script.GetData() is null for
            // an empty script; there is nothing to copy and the cursor does not move.
            return true;
        }

        FMemory::Memcpy(Out, Script.GetData() + Offset, Length);
        Offset += Length;
        return true;
    }

    template <typename T>
    FORCEINLINE bool Read(T& Out)
    {
        static_assert(TIsArithmetic<T>::Value, "Read requires an arithmetic type");
        static_assert(!std::is_same<T, bool>::value, "Read<bool> has no well-defined width; use ReadByte");
        // A truncated operand must still render a reproducible value: zero the destination before the
        // bounds check, so a failed read leaves 0 rather than whatever was on the stack.
        Out = T{};
        return ReadRaw(&Out, sizeof(T));
    }

    template <typename T>
    FORCEINLINE bool ReadPointer(T*& Out)
    {
        Out = nullptr;
        return ReadRaw(static_cast<void*>(&Out), sizeof(T*));
    }

    FORCEINLINE bool Skip(const int64 Count)
    {
        CheckScriptStorage();
        if (IsError())
        {
            return false;
        }

        if (Count < 0 || Offset < 0 || Offset > Script.Num() || Count > static_cast<int64>(Script.Num()) - Offset)
        {
            OnReadError(Offset, Count);
            return false;
        }

        Offset += Count;
        return true;
    }

    template <typename T>
    FArchive& operator<<(T*& Pointer)
    {
        ReadPointer(Pointer);
        return *this;
    }

    template <typename T, typename TEnableIf<TIsArithmetic<T>::Value, int>::Type = 0>
    FArchive& operator<<(T& Value)
    {
        Read(Value);
        return *this;
    }

    FArchive& operator<<(FRotator& Rotator);
    FArchive& operator<<(FScriptName& ScriptName);
    FArchive& operator<<(FString& OutString);

    void Serialize(void* V, const int64 Length) override;

    // The pointer a bytecode operand refers to. Null when the cursor is out of range or fewer than
    // sizeof(T) bytes remain; callers must handle that instead of dereferencing a one-past-the-end
    // pointer. The returned pointer may be unaligned (operands are not padded), so multi-byte reads
    // through it must use unaligned-safe access (FMemory::Memcpy) on platforms that require it.
    template <typename T>
    const T* GetCurrentScriptPointer() const
    {
        CheckScriptStorage();

        // Check the offset before deriving Remaining: with a negative Offset, Num() - Offset is larger
        // than the buffer and the size check would accept a pointer before the start of the script.
        if (Offset < 0 || Offset > static_cast<int64>(Script.Num()) || static_cast<int64>(sizeof(T)) > static_cast<int64>(Script.Num()) - Offset)
        {
            return nullptr;
        }
        return reinterpret_cast<const T*>(Script.GetData() + Offset);
    }

    void Seek(const int64 InOffset) override;
    int64 Tell() override;
    int64 Length() const;
    int64 TotalSize() override;

private:
    FORCEINLINE void CheckScriptStorage() const
    {
        checkf(Script.GetData() == ScriptStorage, TEXT("FBlueprintScriptArchiveReader: the borrowed script was reset or reallocated while the reader was alive"));
    }

    void OnReadError(int64 InOffset, int64 InLength);

    // Trust boundary: the pointer operands this reader returns come straight out of UFunction::Script,
    // which the engine produced when the function was compiled. They are not data from an untrusted
    // package, and the reader does not validate them; callers rely on the script being well-formed
    // (a corrupt script can still yield a stale pointer, which the disassembler's null checks and the
    // visualizer's operand handling tolerate but do not dereference blindly).

    int64 Offset = 0;
    const TArray<uint8>& Script;
    const uint8* const ScriptStorage;
};
