// Copyright (c) 2024-2026 Paul Tankard

#include "BlueprintInspectorVariableEdit.h"

#include "BlueprintInspectorPort.h"

#include "Misc/DefaultValueHelper.h"
#include "Misc/OutputDevice.h"
#if BI_ENGINE_AT_LEAST_5_7
#include "Misc/StringOutputDevice.h"
#else
#include "Containers/UnrealString.h"
#endif
#include "UObject/EnumProperty.h"
#include "UObject/GarbageCollection.h"
#if BI_ENGINE_AT_LEAST_5_5
#include "UObject/StrProperty.h"
#endif
#include "UObject/TextProperty.h"
#include "UObject/UnrealType.h"

namespace BlueprintInspectorVariableEdit
{
namespace
{
#if WITH_DEV_AUTOMATION_TESTS
bool bImportGcGuardHeld = false;

struct FImportGcGuard
{
    FGCScopeGuard Guard;

    FImportGcGuard()
    {
        bImportGcGuardHeld = true;
    }

    ~FImportGcGuard()
    {
        bImportGcGuardHeld = false;
    }
};
#else
using FImportGcGuard = FGCScopeGuard;
#endif

// Struct nesting limit for both the editability check and the value validation, so a recursive struct
// cannot make either walk unbounded.
constexpr int32 MaxTextEditDepth = 16;

ETextEditRejection RejectionForProperty(const FProperty* const Property, const int32 Depth, FString& OutError)
{
    if (Property == nullptr)
    {
        OutError = TEXT("The property does not exist.");
        return ETextEditRejection::NoProperty;
    }
    if (Depth > MaxTextEditDepth)
    {
        OutError = FString::Printf(TEXT("%s nests too deeply to edit as text."), *Property->GetName());
        return ETextEditRejection::UnsupportedType;
    }

    // A const parameter cannot be written through its stack slot.
    if (Property->HasAnyPropertyFlags(CPF_ConstParm))
    {
        OutError = FString::Printf(TEXT("%s is a const parameter and cannot be written."), *Property->GetName());
        return ETextEditRejection::ConstParameter;
    }

    if (Property->HasAnyPropertyFlags(CPF_OutParm | CPF_ReferenceParm))
    {
        OutError = FString::Printf(TEXT("%s is an out or reference parameter and cannot be written."), *Property->GetName());
        return ETextEditRejection::ConstParameter;
    }

    // A fixed-size array reports a per-element size, so importing scalar text into it would write the
    // whole array through a pointer sized for one element.
    if (Property->ArrayDim > 1)
    {
        OutError = FString::Printf(TEXT("%s is a fixed-size array; edit one element instead."), *Property->GetName());
        return ETextEditRejection::FixedSizeArray;
    }

    // v1 excludes values whose text import can load assets or mutate a container. Object/soft
    // references are allowed in v2 because the write is marshalled to the game thread.
    if (Property->IsA<FInterfaceProperty>() || Property->IsA<FDelegateProperty>() || Property->IsA<FMulticastDelegateProperty>() || Property->IsA<FArrayProperty>() || Property->IsA<FSetProperty>() || Property->IsA<FMapProperty>())
    {
        OutError = FString::Printf(TEXT("%s is a %s, which this debugger cannot edit as text."), *Property->GetName(), *Property->GetCPPType());
        return ETextEditRejection::UnsupportedType;
    }

    if (const FStructProperty* const StructProperty = CastField<FStructProperty>(Property))
    {
        if (StructProperty->Struct == nullptr)
        {
            OutError = FString::Printf(TEXT("%s has no script struct to read."), *Property->GetName());
            return ETextEditRejection::UnsupportedType;
        }
        UScriptStruct::ICppStructOps* const Ops = StructProperty->Struct->GetCppStructOps();
        if (Ops != nullptr && Ops->HasImportTextItem())
        {
            return ETextEditRejection::None;
        }
        for (TFieldIterator<FProperty> It(StructProperty->Struct, EFieldIteratorFlags::IncludeSuper); It; ++It)
        {
            FString MemberError;
            const ETextEditRejection MemberRejection = RejectionForProperty(*It, Depth + 1, MemberError);
            if (MemberRejection != ETextEditRejection::None)
            {
                OutError = FString::Printf(TEXT("%s cannot be edited as text: %s"), *Property->GetName(), *MemberError);
                return MemberRejection;
            }
        }
        return ETextEditRejection::None;
    }

    if (Property->IsA<FNumericProperty>() || Property->IsA<FBoolProperty>() || Property->IsA<FEnumProperty>() || Property->IsA<FNameProperty>() || Property->IsA<FStrProperty>() || Property->IsA<FTextProperty>() || Property->IsA<FObjectPropertyBase>())
    {
        return ETextEditRejection::None;
    }

    // No branch may fall through to "editable": an unlisted type is rejected with its own reason.
    OutError = FString::Printf(TEXT("%s is a %s, which this debugger cannot edit as text."), *Property->GetName(), *Property->GetCPPType());
    return ETextEditRejection::UnsupportedType;
}

ETextEditRejection ValidateScalarText(const FProperty* const Property, const FString& Text, FString& OutError)
{
    // The import tolerates surrounding whitespace; validate the same text it will read.
    const FString Trimmed = Text.TrimStartAndEnd();

    // Integer text limits taken from the types themselves, so the literals cannot drift from the
    // platform widths.
    static const FString MaxUInt64Text = FString::Printf(TEXT("%llu"), TNumericLimits<uint64>::Max());
    static const FString MaxInt64Text = FString::Printf(TEXT("%lld"), TNumericLimits<int64>::Max());
    static const FString Int64MinMagnitudeText = FString::Printf(TEXT("%llu"), static_cast<uint64>(TNumericLimits<int64>::Max()) + 1);

    // ImportText accepts out-of-range integer text and wraps/truncates it, and its failure for a bad
    // value comes after parsing. Check the target's limits before the import so the write is rejected
    // with one message. Floating point only needs the overflow/NaN check.
    const auto IntegerTextInRange = [](const TCHAR* InText, const TCHAR* MaxMagnitude) {
        if (*InText == TEXT('\0'))
        {
            return false;
        }
        if (*InText == TEXT('+') || *InText == TEXT('-'))
        {
            ++InText;
        }
        while (*InText == TEXT('0') && *(InText + 1) != TEXT('\0'))
        {
            ++InText;
        }

        int32 Length = 0;
        for (const TCHAR* Cursor = InText; *Cursor != TEXT('\0'); ++Cursor)
        {
            if (!FChar::IsDigit(*Cursor))
            {
                return false;
            }
            ++Length;
        }

        const int32 MaxLength = FCString::Strlen(MaxMagnitude);
        return Length != MaxLength ? Length < MaxLength : FCString::Strcmp(InText, MaxMagnitude) <= 0;
    };
    const auto ValidateLimits = [&Trimmed, &IntegerTextInRange](const FNumericProperty* const Numeric) {
        if (Numeric == nullptr)
        {
            return true;
        }

        if (!Numeric->IsInteger())
        {
            const double Value = FCString::Atod(*Trimmed);
            if (!FMath::IsFinite(Value))
            {
                return false;
            }
            if (CastField<FFloatProperty>(Numeric) != nullptr && FMath::Abs(Value) > static_cast<double>(FLT_MAX))
            {
                return false;
            }
            return true;
        }

        const TCHAR* const Start = *Trimmed;
        if (*Start == TEXT('\0'))
        {
            return false;
        }

        const bool bUnsigned = CastField<FByteProperty>(Numeric) != nullptr || CastField<FUInt16Property>(Numeric) != nullptr || CastField<FUInt32Property>(Numeric) != nullptr || CastField<FUInt64Property>(Numeric) != nullptr;
        if (bUnsigned)
        {
            if (*Start == TEXT('-'))
            {
                return false;
            }
            TCHAR* End = nullptr;
            const uint64 Value = FCString::Strtoui64(Start, &End, 10);
            if (End == Start || *End != TEXT('\0'))
            {
                return false;
            }
            if (CastField<FUInt64Property>(Numeric) != nullptr && !IntegerTextInRange(Start, *MaxUInt64Text))
            {
                return false;
            }
            if (CastField<FByteProperty>(Numeric) != nullptr)
            {
                return Value <= TNumericLimits<uint8>::Max();
            }
            if (CastField<FUInt16Property>(Numeric) != nullptr)
            {
                return Value <= TNumericLimits<uint16>::Max();
            }
            if (CastField<FUInt32Property>(Numeric) != nullptr)
            {
                return Value <= TNumericLimits<uint32>::Max();
            }
            return true;
        }

        TCHAR* End = nullptr;
        const int64 Value = FCString::Strtoi64(Start, &End, 10);
        if (End == Start || *End != TEXT('\0'))
        {
            return false;
        }
        if (CastField<FInt64Property>(Numeric) != nullptr && !IntegerTextInRange(Start, *Start == TEXT('-') ? *Int64MinMagnitudeText : *MaxInt64Text))
        {
            return false;
        }
        if (CastField<FInt8Property>(Numeric) != nullptr)
        {
            return Value >= TNumericLimits<int8>::Min() && Value <= TNumericLimits<int8>::Max();
        }
        if (CastField<FInt16Property>(Numeric) != nullptr)
        {
            return Value >= TNumericLimits<int16>::Min() && Value <= TNumericLimits<int16>::Max();
        }
        if (CastField<FIntProperty>(Numeric) != nullptr)
        {
            return Value >= TNumericLimits<int32>::Min() && Value <= TNumericLimits<int32>::Max();
        }
        return true;
    };

    // A numeric enum value must name a declared enumerator: ImportText accepts any in-range number, which
    // leaves the Blueprint holding a value its switches and array indexing do not expect. Bitflags combine
    // entries by design, and a build without editor-only metadata cannot read the enumerators, so both
    // keep the range check alone. A value the int64 parser rejects is outside the enum's value domain.
    const auto IsDeclaredEnumValue = [](const UEnum* const Enum, const FString& InText) -> bool {
        if (Enum == nullptr)
        {
            return true;
        }
#if WITH_EDITORONLY_DATA
        constexpr bool bDeclaredEnumeratorsReadable = true;
        const bool bBitflags = Enum->HasMetaData(TEXT("Bitflags"));
#else
        constexpr bool bDeclaredEnumeratorsReadable = false;
        const bool bBitflags = false;
#endif
        int64 Value = 0;
        const bool bParsed = FDefaultValueHelper::ParseInt64(InText, Value);
        const bool bDeclared = bParsed && Enum->IsValidEnumValue(Value);
        FEnumValueAcceptance Acceptance;
        Acceptance.bBitflags = bBitflags;
        Acceptance.bDeclaredEnumeratorsReadable = bDeclaredEnumeratorsReadable;
        Acceptance.bParsed = bParsed;
        Acceptance.bDeclared = bDeclared;
        Acceptance.bGeneratedMax = bDeclared && Enum->ContainsExistingMax() && Enum->GetIndexByValue(Value) == Enum->NumEnums() - 1;
        return AcceptsNumericEnumValue(Acceptance);
    };

    if (const FBoolProperty* const BoolProperty = CastField<FBoolProperty>(Property))
    {
        const bool bValid = Trimmed.Equals(TEXT("true"), ESearchCase::IgnoreCase) || Trimmed.Equals(TEXT("false"), ESearchCase::IgnoreCase) || Trimmed == TEXT("1") || Trimmed == TEXT("0");
        if (!bValid)
        {
            OutError = FString::Printf(TEXT("'%s' is not a valid bool."), *Text);
            return ETextEditRejection::InvalidValue;
        }
        return ETextEditRejection::None;
    }

    if (const FEnumProperty* const EnumProperty = CastField<FEnumProperty>(Property))
    {
        const UEnum* const Enum = EnumProperty->GetEnum();
        const int32 NameIndex = Enum != nullptr ? Enum->GetIndexByNameString(Trimmed) : INDEX_NONE;
        const bool bGeneratedMaxName = NameIndex != INDEX_NONE && Enum->ContainsExistingMax() && NameIndex == Enum->NumEnums() - 1;
        const bool bName = NameIndex != INDEX_NONE && !bGeneratedMaxName;
        const bool bValid = bName || (FDefaultValueHelper::IsStringValidInteger(Trimmed) && ValidateLimits(EnumProperty->GetUnderlyingProperty()) && IsDeclaredEnumValue(Enum, Trimmed));
        if (!bValid)
        {
            OutError = FString::Printf(TEXT("'%s' is not a valid %s."), *Text, *Property->GetName());
            return ETextEditRejection::InvalidValue;
        }
        return ETextEditRejection::None;
    }

    if (const FByteProperty* const ByteProperty = CastField<FByteProperty>(Property))
    {
        if (ByteProperty->Enum != nullptr)
        {
            const int32 NameIndex = ByteProperty->Enum->GetIndexByNameString(Trimmed);
            const bool bGeneratedMaxName = NameIndex != INDEX_NONE && ByteProperty->Enum->ContainsExistingMax() && NameIndex == ByteProperty->Enum->NumEnums() - 1;
            const bool bName = NameIndex != INDEX_NONE && !bGeneratedMaxName;
            const bool bValid = bName || (FDefaultValueHelper::IsStringValidInteger(Trimmed) && ValidateLimits(ByteProperty) && IsDeclaredEnumValue(ByteProperty->Enum, Trimmed));
            if (!bValid)
            {
                OutError = FString::Printf(TEXT("'%s' is not a valid %s."), *Text, *Property->GetName());
                return ETextEditRejection::InvalidValue;
            }
            return ETextEditRejection::None;
        }
    }

    if (const FNumericProperty* const NumericProperty = CastField<FNumericProperty>(Property))
    {
        const bool bValid = (NumericProperty->IsInteger() ? FDefaultValueHelper::IsStringValidInteger(Trimmed) : FDefaultValueHelper::IsStringValidFloat(Trimmed)) && ValidateLimits(NumericProperty);
        if (!bValid)
        {
            OutError = FString::Printf(TEXT("'%s' is not a valid %s."), *Text, *Property->GetName());
            return ETextEditRejection::InvalidValue;
        }
        return ETextEditRejection::None;
    }

    // Name, string, text and object references accept any text the import will read; the import's own
    // remainder check is what rejects garbage. A type outside that set is rejected here rather than
    // being handed to ImportText unvalidated.
    if (Property->IsA<FNameProperty>() || Property->IsA<FStrProperty>() || Property->IsA<FTextProperty>() || Property->IsA<FObjectPropertyBase>())
    {
        return ETextEditRejection::None;
    }

    OutError = FString::Printf(TEXT("'%s' cannot be validated as a %s."), *Text, *Property->GetCPPType());
    return ETextEditRejection::UnsupportedType;
}

// Validates a struct literal member by member, mirroring the grammar ImportText accepts: parenthesised
// fields, quoted values with backslash escapes, and nested parenthesised struct values. The import
// would wrap or truncate an out-of-range member before its own error path, so the member text is
// checked first and rejected with the member's own message.
ETextEditRejection ValidateStructText(const FStructProperty& StructProperty, const FString& Text, FString& OutError, const int32 Depth)
{
    if (Depth > MaxTextEditDepth)
    {
        OutError = FString::Printf(TEXT("%s nests too deeply to validate."), *StructProperty.GetName());
        return ETextEditRejection::InvalidValue;
    }
    if (StructProperty.Struct == nullptr)
    {
        OutError = FString::Printf(TEXT("%s has no script struct to read."), *StructProperty.GetName());
        return ETextEditRejection::UnsupportedType;
    }

    UScriptStruct::ICppStructOps* const Ops = StructProperty.Struct->GetCppStructOps();
    if (Ops != nullptr && Ops->HasImportTextItem())
    {
        return ETextEditRejection::None;
    }

    const FString Trimmed = Text.TrimStartAndEnd();
    const int32 Length = Trimmed.Len();
    if (Length < 2 || Trimmed[0] != TEXT('(') || Trimmed[Length - 1] != TEXT(')'))
    {
        OutError = FString::Printf(TEXT("'%s' is not a %s value."), *Text, *StructProperty.Struct->GetName());
        return ETextEditRejection::InvalidValue;
    }

    // Index-based (not pointer-based) slicing: FString's two-argument constructor is a slack hint in UE5,
    // not a length, so a pointer constructor would silently take the rest of the string.
    int32 Cursor = 1;
    const int32 End = Length - 1; // the closing ')'
    while (Cursor < End)
    {
        while (Cursor < End && FChar::IsWhitespace(Trimmed[Cursor]))
        {
            ++Cursor;
        }
        if (Cursor < End && Trimmed[Cursor] == TEXT(','))
        {
            ++Cursor;
            continue;
        }
        if (Cursor >= End)
        {
            break;
        }

        // A field name is never quoted; it runs up to '='.
        const int32 NameStart = Cursor;
        while (Cursor < End && Trimmed[Cursor] != TEXT('=') && Trimmed[Cursor] != TEXT(',') && Trimmed[Cursor] != TEXT(')'))
        {
            ++Cursor;
        }
        if (Cursor >= End || Trimmed[Cursor] != TEXT('='))
        {
            OutError = FString::Printf(TEXT("'%s' is not a %s value: a field has no '='."), *Text, *StructProperty.Struct->GetName());
            return ETextEditRejection::InvalidValue;
        }
        const FString MemberName = Trimmed.Mid(NameStart, Cursor - NameStart).TrimStartAndEnd();
        ++Cursor; // '='

        // A quoted value may hold commas and parentheses; a nested value holds balanced pairs.
        // Everything else ends at the next top-level ',' or at the struct's closing ')'.
        const int32 ValueStart = Cursor;
        int32 Nesting = 0;
        TCHAR Quote = 0;
        bool bEscaped = false;
        for (; Cursor < End; ++Cursor)
        {
            const TCHAR Character = Trimmed[Cursor];
            if (Quote != 0)
            {
                if (bEscaped)
                {
                    bEscaped = false;
                }
                else if (Character == TEXT('\\'))
                {
                    bEscaped = true;
                }
                else if (Character == Quote)
                {
                    Quote = 0;
                }
                continue;
            }
            if (Character == TEXT('"') || Character == TEXT('\''))
            {
                Quote = Character;
                continue;
            }
            if (Character == TEXT('(') || Character == TEXT('{') || Character == TEXT('['))
            {
                ++Nesting;
                continue;
            }
            if (Character == TEXT(')') || Character == TEXT('}') || Character == TEXT(']'))
            {
                if (Nesting == 0)
                {
                    break;
                }
                --Nesting;
                continue;
            }
            if (Character == TEXT(',') && Nesting == 0)
            {
                break;
            }
        }
        if (Quote != 0 || Nesting != 0)
        {
            OutError = FString::Printf(TEXT("'%s' is not a %s value: a member value is malformed."), *Text, *StructProperty.Struct->GetName());
            return ETextEditRejection::InvalidValue;
        }
        const FString ValueText = Trimmed.Mid(ValueStart, Cursor - ValueStart).TrimStartAndEnd();

        FProperty* const Member = StructProperty.Struct->FindPropertyByName(FName(*MemberName));
        if (Member == nullptr)
        {
            OutError = FString::Printf(TEXT("'%s' is not a member of %s."), *MemberName, *StructProperty.Struct->GetName());
            return ETextEditRejection::InvalidValue;
        }

        ETextEditRejection MemberRejection = ETextEditRejection::None;
        if (const FStructProperty* const MemberStruct = CastField<FStructProperty>(Member))
        {
            MemberRejection = ValidateStructText(*MemberStruct, ValueText, OutError, Depth + 1);
        }
        else
        {
            MemberRejection = ValidateScalarText(Member, ValueText, OutError);
        }
        if (MemberRejection != ETextEditRejection::None)
        {
            return MemberRejection;
        }

        if (Cursor < End && Trimmed[Cursor] == TEXT(','))
        {
            ++Cursor;
        }
    }

    return ETextEditRejection::None;
}

ETextEditRejection ValidatePropertyText(const FProperty* const Property, const FString& Text, FString& OutError, const int32 Depth)
{
    if (const FStructProperty* const StructProperty = CastField<FStructProperty>(Property))
    {
        return ValidateStructText(*StructProperty, Text, OutError, Depth);
    }
    return ValidateScalarText(Property, Text, OutError);
}
} // namespace

bool AcceptsNumericEnumValue(const FEnumValueAcceptance& Value)
{
    if (Value.bBitflags || !Value.bDeclaredEnumeratorsReadable)
    {
        return true;
    }
    return Value.bParsed && Value.bDeclared && !Value.bGeneratedMax;
}

ETextEditRejection GetTextEditRejection(const FProperty* const Property, FString& OutError)
{
    OutError.Reset();

    // Reads property metadata the editor can mutate, so the same thread rule as Apply applies. Reporting
    // the threading reason keeps a misrouted call from looking like a property-type rejection.
    if (!IsInGameThread())
    {
        OutError = TEXT("Variable writes must run on the game thread.");
        return ETextEditRejection::OffGameThread;
    }
    return RejectionForProperty(Property, 0, OutError);
}

bool IsTextEditable(const FProperty* const Property, FString& OutError)
{
    return GetTextEditRejection(Property, OutError) == ETextEditRejection::None;
}

bool IsTextEditable(const FProperty* const Property)
{
    FString Ignored;
    return IsTextEditable(Property, Ignored);
}

bool Apply(const FProperty* const Property, uint8* const Container, UObject* const Owner, const FString& Text, FString& OutValue, FString& OutError)
{
    OutValue.Reset();
    OutError.Reset();

    // Every write mutates a live object, and an object-reference import can load assets on the calling
    // thread. Refuse off-thread rather than race the VM; the subsystem marshals its writes.
    if (!IsInGameThread())
    {
        OutError = TEXT("Variable writes must run on the game thread.");
        return false;
    }

    if (Property == nullptr)
    {
        OutError = TEXT("There is no property to edit.");
        return false;
    }
    if (Container == nullptr)
    {
        OutError = TEXT("The property has no storage.");
        return false;
    }

    FString RejectionError;
    if (GetTextEditRejection(Property, RejectionError) != ETextEditRejection::None)
    {
        OutError = RejectionError;
        return false;
    }
    if (ValidatePropertyText(Property, Text, OutError, 0) != ETextEditRejection::None)
    {
        return false;
    }

    void* const Value = Property->ContainerPtrToValuePtr<void>(Container);

    // Import into scratch storage first. ImportText writes fields as it parses, so a struct or a
    // multi-field value can be partially written before a later field fails; the live property must
    // only change once the whole text parsed. The scratch block is initialised like a real property
    // and aligned to the property's minimum alignment, because ImportText may touch vectors or
    // transforms. The owner destroys the temporary value on every exit path, including a failed
    // allocation, where check(Scratch != nullptr) would be compiled out in Shipping/Test.
    struct FScratchValue
    {
        FScratchValue(const FProperty* const InProperty, const void* const InValue) : Property(InProperty), Memory(FMemory::Malloc(InProperty->GetSize(), InProperty->GetMinAlignment()))
        {
            if (Memory != nullptr)
            {
                Property->InitializeValue(Memory);
                Property->CopyCompleteValue(Memory, InValue);
            }
        }

        ~FScratchValue()
        {
            if (Memory != nullptr)
            {
                Property->DestroyValue(Memory);
                FMemory::Free(Memory);
            }
        }

        FScratchValue(const FScratchValue&) = delete;
        FScratchValue& operator=(const FScratchValue&) = delete;

        const FProperty* const Property;
        void* const Memory;
    };

    FImportGcGuard ImportGcGuard;
    FScratchValue Scratch(Property, Value);
    if (Scratch.Memory == nullptr)
    {
        OutError = FString::Printf(TEXT("%s could not be edited: out of memory."), *Property->GetName());
        return false;
    }

    // 4.x logs struct parse failures through ErrorText without a null check
    // (`UScriptStruct::ImportText` calls `ErrorText->Logf` on every error path), so a null device
    // turns a rejected value into an editor crash. Pass a real device on every engine and keep its
    // text, which is the only explanation the 4.x importer produces.
    FStringOutputDevice ImportErrors;
    const TCHAR* Consumed = nullptr;
#if BI_ENGINE_AT_LEAST_5_5
    Consumed = Property->ImportText_Direct(*Text, Scratch.Memory, Owner, PPF_PropertyWindow | PPF_BlueprintDebugView, &ImportErrors);
#else
    Consumed = Property->ImportText(*Text, Scratch.Memory, PPF_PropertyWindow, Owner, &ImportErrors);
#endif

    // ImportText returns the first unconsumed character, not null on a partial parse: `12abc`,
    // `"abc" trailing` and `(X=1) garbage` all return a pointer. Accept the text only when the
    // remainder is whitespace, or a struct value could be committed while half of it failed.
    const TCHAR* Remainder = Consumed;
    while (Remainder != nullptr && *Remainder != TEXT('\0') && FChar::IsWhitespace(*Remainder))
    {
        ++Remainder;
    }
    if (Consumed == nullptr || (Remainder != nullptr && *Remainder != TEXT('\0')))
    {
        OutError = FString::Printf(TEXT("'%s' is not a valid %s."), *Text, *Property->GetName());
        const FString EngineError = ImportErrors.TrimStartAndEnd();
        if (!EngineError.IsEmpty())
        {
            OutError += TEXT(" ") + EngineError;
        }
        return false;
    }

    Property->CopyCompleteValue(Value, Scratch.Memory);

#if BI_ENGINE_AT_LEAST_5_4
    Property->ExportTextItem_Direct(OutValue, Value, nullptr, Owner, PPF_PropertyWindow | PPF_BlueprintDebugView, nullptr);
#else
    Property->ExportTextItem(OutValue, Value, nullptr, Owner, PPF_PropertyWindow | PPF_BlueprintDebugView, nullptr);
#endif
    return true;
}
#if WITH_DEV_AUTOMATION_TESTS
bool IsImportGcGuardHeldForTest()
{
    return bImportGcGuardHeld;
}
#endif
} // namespace BlueprintInspectorVariableEdit
