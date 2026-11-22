// Copyright (c) 2024-2026 Paul Tankard

#include "BlueprintStructDefaults.h"

#if WITH_EDITOR

#include "BlueprintInspectorPort.h"

#if BI_ENGINE_AT_LEAST_5_5
#include "StructUtils/UserDefinedStruct.h"
#else
#include "Engine/UserDefinedStruct.h"
#endif
#include "Kismet2/StructureEditorUtils.h"
#include "UObject/UnrealType.h"
#include "UserDefinedStructure/UserDefinedStructEditorData.h"

namespace BlueprintStructDefaults
{
constexpr int32 MaxRewriteDepth = 8;

FString MemberBaseName(const FString& Name)
{
    // Struct literals spell a member as "<name>_<index>_<guid>". Only strip the suffix when the tail
    // is exactly a 32-character GUID; a plain name ending in digits must remain untouched. Mid() from
    // GuidSeparator + 1 always yields 32 characters, so the length is not re-checked here.
    const int32 GuidSeparator = Name.Len() - 33;
    if (GuidSeparator > 0 && Name[GuidSeparator] == TEXT('_'))
    {
        const FString Guid = Name.Mid(GuidSeparator + 1);
        bool bAllHex = true;
        for (const TCHAR Character : Guid)
        {
            bAllHex = bAllHex && FChar::IsHexDigit(Character);
        }
        if (bAllHex)
        {
            int32 NumberEnd = GuidSeparator;
            int32 NumberStart = NumberEnd;
            while (NumberStart > 0 && FChar::IsDigit(Name[NumberStart - 1]))
            {
                --NumberStart;
            }
            if (NumberStart < NumberEnd && NumberStart > 0 && Name[NumberStart - 1] == TEXT('_'))
            {
                return Name.Left(NumberStart - 1);
            }
        }
    }
    return Name;
}

bool TryTargetStructMemberName(const UScriptStruct* const Struct, const FString& ArchiveName, FString& OutName)
{
    const FString Base = MemberBaseName(ArchiveName);
    if (Struct != nullptr)
    {
        if (const UUserDefinedStruct* const UserStruct = Cast<UUserDefinedStruct>(Struct))
        {
            for (const FStructVariableDescription& Variable : FStructureEditorUtils::GetVarDesc(UserStruct))
            {
                if (MemberBaseName(Variable.VarName.ToString()).Equals(Base, ESearchCase::IgnoreCase))
                {
                    OutName = Variable.VarName.ToString();
                    return true;
                }
            }
        }
        for (TFieldIterator<FProperty> It(Struct); It; ++It)
        {
            if (MemberBaseName(It->GetName()).Equals(Base, ESearchCase::IgnoreCase))
            {
                OutName = It->GetName();
                return true;
            }
        }
    }
    OutName = Base;
    return false;
}

FString TargetStructMemberName(const UScriptStruct* const Struct, const FString& ArchiveName)
{
    FString Name;
    TryTargetStructMemberName(Struct, ArchiveName, Name);
    return Name;
}

bool SplitStructMembers(const FString& Value, TArray<FString>& OutMembers)
{
    OutMembers.Reset();
    if (Value.Len() < 2 || !Value.StartsWith(TEXT("(")) || !Value.EndsWith(TEXT(")")))
    {
        return false;
    }

    int32 Depth = 0;
    bool bInQuotes = false;
    bool bEscaped = false;
    FString Current;
    for (int32 Index = 1; Index < Value.Len() - 1; ++Index)
    {
        const TCHAR Character = Value[Index];
        if (bEscaped)
        {
            // The backslash was already appended; nothing in an escaped pair can toggle or separate.
            bEscaped = false;
        }
        else if (bInQuotes && Character == TEXT('\\'))
        {
            bEscaped = true;
        }
        else if (Character == TEXT('"'))
        {
            bInQuotes = !bInQuotes;
        }
        else if (!bInQuotes && Character == TEXT('('))
        {
            ++Depth;
        }
        else if (!bInQuotes && Character == TEXT(')'))
        {
            if (Depth == 0)
            {
                // A stray ')' means the literal is malformed; a negative depth would corrupt the split.
                OutMembers.Reset();
                return false;
            }
            --Depth;
        }
        else if (!bInQuotes && Depth == 0 && Character == TEXT(','))
        {
            OutMembers.Add(Current);
            Current.Reset();
            continue;
        }
        Current.AppendChar(Character);
    }

    if (Depth != 0 || bInQuotes)
    {
        // A truncated/unbalanced literal (including an unterminated quote) must fail rather than
        // produce a plausible-looking split.
        OutMembers.Reset();
        return false;
    }

    OutMembers.Add(Current);
    return true;
}

// A member whose type is a struct (directly, or inside an array/set/map) still carries archive member
// names that need rewriting. Unwrap the container chain to the inner struct property.
const UScriptStruct* FindInnerStruct(const FProperty* const Property)
{
    const FProperty* Current = Property;
    while (Current != nullptr)
    {
        if (const FArrayProperty* const Array = CastField<FArrayProperty>(Current))
        {
            Current = Array->Inner;
            continue;
        }
        if (const FSetProperty* const Set = CastField<FSetProperty>(Current))
        {
            Current = Set->ElementProp;
            continue;
        }
        if (const FMapProperty* const Map = CastField<FMapProperty>(Current))
        {
            Current = Map->ValueProp;
            continue;
        }
        break;
    }

    const FStructProperty* const StructProperty = CastField<FStructProperty>(Current);
    return StructProperty != nullptr ? StructProperty->Struct : nullptr;
}

FString RewriteMapDefault(const FMapProperty* const Map, const FString& Value, const int32 Depth)
{
    TArray<FString> Elements;
    if (Map == nullptr || Depth > MaxRewriteDepth || !SplitStructMembers(Value, Elements))
    {
        return Value;
    }

    const UScriptStruct* const KeyStruct = FindInnerStruct(Map->KeyProp);
    const UScriptStruct* const ValueStruct = FindInnerStruct(Map->ValueProp);

    TArray<FString> Rewritten;
    Rewritten.Reserve(Elements.Num());
    for (const FString& Element : Elements)
    {
        TArray<FString> Pair;
        if (!SplitStructMembers(Element, Pair) || Pair.Num() != 2)
        {
            Rewritten.Add(Element);
            continue;
        }

        FString KeyText = Pair[0].TrimStartAndEnd();
        FString ValueText = Pair[1].TrimStartAndEnd();
        if (KeyStruct != nullptr)
        {
            KeyText = RewriteStructDefault(KeyStruct, KeyText, Depth + 1);
        }
        if (ValueStruct != nullptr)
        {
            ValueText = RewriteStructDefault(ValueStruct, ValueText, Depth + 1);
        }
        Rewritten.Add(TEXT("(") + KeyText + TEXT(",") + ValueText + TEXT(")"));
    }

    return TEXT("(") + FString::Join(Rewritten, TEXT(",")) + TEXT(")");
}

FString RewriteStructDefault(const UScriptStruct* const Struct, const FString& Value, const int32 Depth)
{
    TArray<FString> Members;
    if (Struct == nullptr || Depth > MaxRewriteDepth || !SplitStructMembers(Value, Members))
    {
        return Value;
    }

    TArray<FString> Rewritten;
    Rewritten.Reserve(Members.Num());
    for (const FString& Member : Members)
    {
        // An element of an array/set literal is not a named member; rewrite it against the same struct.
        const FString TrimmedMember = Member.TrimStartAndEnd();
        if (TrimmedMember.StartsWith(TEXT("(")))
        {
            Rewritten.Add(RewriteStructDefault(Struct, TrimmedMember, Depth + 1));
            continue;
        }

        FString Key;
        FString MemberValue;
        if (!TrimmedMember.Split(TEXT("="), &Key, &MemberValue, ESearchCase::CaseSensitive, ESearchDir::FromStart))
        {
            Rewritten.Add(Member);
            continue;
        }

        // A loosely formatted literal can carry spaces around the key; trim before the lookup so the
        // property still resolves. An unresolved key keeps the archive spelling instead of a stripped
        // base name that would be silently dropped by the compiler.
        const FString TrimmedKey = Key.TrimStartAndEnd();
        const FString TrimmedValue = MemberValue.TrimStartAndEnd();
        FString TargetName;
        const bool bResolved = TryTargetStructMemberName(Struct, TrimmedKey, TargetName);
        const FString UseName = bResolved ? TargetName : TrimmedKey;
        const FStructProperty* const Property = FindFProperty<FStructProperty>(Struct, *UseName);
        if (Property != nullptr)
        {
            MemberValue = RewriteStructDefault(Property->Struct, TrimmedValue, Depth + 1);
        }
        else if (const FProperty* const MemberProperty = FindFProperty<FProperty>(Struct, *UseName))
        {
            if (const FMapProperty* const MapProperty = CastField<FMapProperty>(MemberProperty))
            {
                MemberValue = RewriteMapDefault(MapProperty, TrimmedValue, Depth + 1);
            }
            else if (const UScriptStruct* const InnerStruct = FindInnerStruct(MemberProperty))
            {
                MemberValue = RewriteStructDefault(InnerStruct, TrimmedValue, Depth + 1);
            }
        }
        Rewritten.Add(UseName + TEXT("=") + MemberValue);
    }

    return TEXT("(") + FString::Join(Rewritten, TEXT(",")) + TEXT(")");
}
} // namespace BlueprintStructDefaults

#endif // WITH_EDITOR
