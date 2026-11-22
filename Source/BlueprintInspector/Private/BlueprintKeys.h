#pragma once

#include "CoreMinimal.h"

inline constexpr TCHAR PiePackagePrefixChars[] = TEXT("UEDPIE_");
inline constexpr int32 PiePackagePrefixLength = static_cast<int32>(UE_ARRAY_COUNT(PiePackagePrefixChars) - 1);
inline constexpr const TCHAR* PiePackagePrefix()
{
    return PiePackagePrefixChars;
}

// One key shape per lookup: an address inside a named function, or a break/continue arm's jump/landing
// pair. The tag keeps otherwise identical keys distinct so a breakpoint map and a capture map cannot be
// confused even though they hash the same fields.
template <typename Tag>
struct FBlueprintAddressKey
{
    // Must hold the same identifier on insert and lookup: the class *path* produced by
    // GetFunctionClassPath(). Do not mix in class names or display names.
    FName ClassPath;
    FName FunctionName;
    int64 Address = 0;

    bool operator==(const FBlueprintAddressKey& Other) const
    {
        return ClassPath == Other.ClassPath && FunctionName == Other.FunctionName && Address == Other.Address;
    }
};

template <typename Tag>
FORCEINLINE uint32 GetTypeHash(const FBlueprintAddressKey<Tag>& Key)
{
    uint32 Hash = HashCombine(GetTypeHash(Key.ClassPath), GetTypeHash(Key.FunctionName));
    return HashCombine(Hash, GetTypeHash(Key.Address));
}

template <typename Tag>
struct FBlueprintTransferKey
{
    // Same contract as FBlueprintAddressKey::ClassPath: the PIE-normalized class path only.
    FName ClassPath;
    FName FunctionName;
    int64 JumpAddress = 0;
    int64 Landing = 0;

    bool operator==(const FBlueprintTransferKey& Other) const
    {
        return ClassPath == Other.ClassPath && FunctionName == Other.FunctionName && JumpAddress == Other.JumpAddress && Landing == Other.Landing;
    }
};

template <typename Tag>
FORCEINLINE uint32 GetTypeHash(const FBlueprintTransferKey<Tag>& Key)
{
    uint32 Hash = HashCombine(GetTypeHash(Key.ClassPath), GetTypeHash(Key.FunctionName));
    Hash = HashCombine(Hash, GetTypeHash(Key.JumpAddress));
    return HashCombine(Hash, GetTypeHash(Key.Landing));
}

struct FBreakpointKeyTag
{
};
struct FBreakpointTransferKeyTag
{
};
struct FHotHitKeyTag
{
};
struct FHotTransferKeyTag
{
};
struct FTransferNameKeyTag
{
};
struct FSourcePathKeyTag
{
};

using FBreakpointKey = FBlueprintAddressKey<FBreakpointKeyTag>;
using FBreakpointTransferKey = FBlueprintTransferKey<FBreakpointTransferKeyTag>;
using FBlueprintHotHitKey = FBlueprintAddressKey<FHotHitKeyTag>;
using FBlueprintHotTransferKey = FBlueprintTransferKey<FHotTransferKeyTag>;
using FBlueprintTransferNameKey = FBlueprintTransferKey<FTransferNameKeyTag>;
using FBlueprintSourcePathKey = FBlueprintAddressKey<FSourcePathKeyTag>;
