#pragma once

#include "CoreMinimal.h"

class FProperty;
class UClass;
class UFunction;
class UObject;
class UScriptStruct;

// One frame of the paused call chain. Frame points into the VM stack and stays valid until the pause
// is released, which is the only window in which the DAP handlers read it. The frames are ordered with
// the stopped function first; DAP frame id N is PausedFrames[N - 1].
//
// The pointers and addresses here are raw and not GC-visible. They are valid only while the game
// thread is parked and the frame is published; FBlueprintPauseSession::ClearFrame() runs under
// FBlueprintPauseSession::Lock and bumps the generation before the VM resumes. A consumer must not retain
// a frame, or a FBlueprintVariableGroup copied from one, past the resume: the stack and locals it points
// into are gone and the UFunction may be collected.
struct FBlueprintPausedFrame
{
    const UFunction* Function = nullptr;
    const UClass* Class = nullptr;
    int64 Address = 0;
    int32 ClassHandle = 0;
    int32 Line = 1;
    int32 ParametersReference = 0;
    int32 LocalsReference = 0;
    int32 ClassReference = 0;
};

enum class EBlueprintVariableGroupKind : uint8
{
    Struct,
    Parameters,
    Locals,
    Class,
    Array,
    Set,
    Map,
};

// Whether a group's variables can be written. A group that points into a set element is read-only:
// the set hashes its elements, so editing a member in place would corrupt the table.
enum class EBlueprintVariableGroupAccess : uint8
{
    Writable,
    ReadOnly,
};

struct FBlueprintVariableGroup
{
    EBlueprintVariableGroupKind Kind = EBlueprintVariableGroupKind::Struct;
    const UScriptStruct* Struct = nullptr;
    const uint8* Address = nullptr;
    const UFunction* Function = nullptr;
    const uint8* Locals = nullptr;
    const UObject* Object = nullptr;
    const FProperty* Property = nullptr;

    EBlueprintVariableGroupAccess Access = EBlueprintVariableGroupAccess::Writable;
    uint64 PauseGeneration = 0;

    bool IsLive(const uint64 CurrentGeneration) const
    {
        return PauseGeneration == CurrentGeneration;
    }
};
