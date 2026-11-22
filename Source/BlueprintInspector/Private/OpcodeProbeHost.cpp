// Copyright (c) 2024-2026 Paul Tankard

#include "OpcodeProbeHost.h"

#if WITH_DEV_AUTOMATION_TESTS
#include "BlueprintInspectorVariableEdit.h"
#endif

namespace
{
bool bGcImportSawLocked = false;
}

namespace OpcodeProbeGcImport
{
void ResetProbe()
{
    bGcImportSawLocked = false;
}

bool SawGarbageCollectionLocked()
{
    return bGcImportSawLocked;
}
} // namespace OpcodeProbeGcImport

bool FProbeGcImportStruct::ImportTextItem(const TCHAR*& Buffer, const int32 PortFlags, UObject* Owner, FOutputDevice* ErrorText)
{
    (void)PortFlags;
    (void)Owner;
    (void)ErrorText;
#if WITH_DEV_AUTOMATION_TESTS
    bGcImportSawLocked = BlueprintInspectorVariableEdit::IsImportGcGuardHeldForTest();
#endif

    const TCHAR* Cursor = Buffer;
    while (Cursor != nullptr && FChar::IsWhitespace(*Cursor))
    {
        ++Cursor;
    }
    if (Cursor == nullptr || FCString::Strncmp(Cursor, TEXT("gc"), 2) != 0)
    {
        return false;
    }
    Buffer = Cursor + 2;
    return true;
}

void UOpcodeProbeHost::TakeFloat(float Value)
{
    // The function exists only as a call target for the opcode fixtures; the VM passes the value and
    // the body intentionally ignores it.
    (void)Value;
}

float UOpcodeProbeHost::GetSinglePrecision()
{
    // Mirror FloatVar's default rather than duplicating the literal, so the probe and the property stay
    // in sync if the default changes.
    return GetDefault<UOpcodeProbeHost>()->FloatVar;
}

void UOpcodeProbeHost::TakeCaller(const UObject* Caller, const int32 Value)
{
    (void)Caller;
    (void)Value;
}
