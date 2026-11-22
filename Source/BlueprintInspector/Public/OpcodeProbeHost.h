// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"
#include "Math/IntPoint.h"
#include "Math/Vector.h"
#include "UObject/Class.h"
#include "UObject/Object.h"
#include "UObject/PrimaryAssetId.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/SoftObjectPtr.h"

#include "OpcodeProbeHost.generated.h"

/** A single-cast signature declared natively: a Blueprint-authored one does not survive saving. */
DECLARE_DYNAMIC_DELEGATE_OneParam(FProbeSingleDelegateSignature, int32, Value);

/**
 * Backs the variable-edit GC-guard test: the import records whether it ran while garbage collection
 * was locked, which is what keeps an object loaded into scratch storage alive until it is committed.
 */
USTRUCT()
struct BLUEPRINTINSPECTOR_API FProbeGcImportStruct
{
    GENERATED_BODY()

    bool ImportTextItem(const TCHAR*& Buffer, int32 PortFlags, UObject* Owner, FOutputDevice* ErrorText);
};

template <>
struct TStructOpsTypeTraits<FProbeGcImportStruct> : public TStructOpsTypeTraitsBase2<FProbeGcImportStruct>
{
    enum
    {
        WithImportTextItem = true
    };
};

namespace OpcodeProbeGcImport
{
BLUEPRINTINSPECTOR_API void ResetProbe();
BLUEPRINTINSPECTOR_API bool SawGarbageCollectionLocked();
} // namespace OpcodeProbeGcImport

/**
 * Backs the variable-edit rejection tests: a struct is editable only when every member is, and the map
 * member is the one that must be named in the rejection. The soft reference is editable on its own.
 */
USTRUCT()
struct BLUEPRINTINSPECTOR_API FProbeUnsupportedStruct
{
    GENERATED_BODY()

    UPROPERTY()
    TMap<FName, int32> MapVar;

    UPROPERTY()
    TSoftObjectPtr<UObject> SoftVar;
};

/** A string and an int, so a comma inside a quoted member value must stay inside that member. */
USTRUCT()
struct BLUEPRINTINSPECTOR_API FProbeCommaStruct
{
    GENERATED_BODY()

    UPROPERTY()
    FString TextVar;

    UPROPERTY()
    int32 NumberVar = 0;
};

UCLASS(BlueprintType)
class BLUEPRINTINSPECTOR_API UOpcodeProbeHost : public UObject
{
    GENERATED_BODY()

public:
    /** Mirrors the CDO value GetSinglePrecision() reads; the 0.5f default drives the opcode fixtures. */
    UPROPERTY(BlueprintReadWrite, Category = "Opcode probe")
    float FloatVar = 0.5f;

    /** Not Blueprint-exposed: Blueprint has no unsigned 64-bit integer type. */
    UPROPERTY()
    uint64 UInt64Var = 0;

    /**
     * Reflected as FVector on every engine: UE4.25's FVector is the float vector, while UE5's is double
     * precision (FVector3f cannot be reflected by UHT). The name is historical; do not assume single
     * precision.
     */
    UPROPERTY()
    FVector Vector3fVar = FVector::ZeroVector;

    /** int32 members on every engine, so an out-of-range member test does not depend on float width. */
    UPROPERTY()
    FIntPoint IntPointVar = FIntPoint(0, 0);

    UPROPERTY()
    FPrimaryAssetId PrimaryAssetIdVar;

    /** Holds a map member, so the variable-edit gate must reject it and name the member. */
    UPROPERTY()
    FProbeUnsupportedStruct UnsupportedStructVar;

    UPROPERTY()
    FProbeCommaStruct CommaStructVar;

    UPROPERTY()
    FProbeGcImportStruct GcImportVar;

    /** Backs the single-cast delegate fixtures; the signature above gives it a stable native type. */
    UPROPERTY(BlueprintReadWrite, Category = "Opcode probe")
    FProbeSingleDelegateSignature SingleDelegateVar;

    UFUNCTION(BlueprintCallable, Category = "Opcode probe")
    static void TakeFloat(float Value);

    /** Feeding this into a real pin is what produces CST_FloatToDouble. */
    UFUNCTION(BlueprintCallable, Category = "Opcode probe")
    static float GetSinglePrecision();

    UFUNCTION(BlueprintCallable, Category = "Opcode probe")
    static void TakeCaller(const UObject* Caller, int32 Value);
};
