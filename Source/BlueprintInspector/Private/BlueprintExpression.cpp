// Copyright (c) 2024-2026 Paul Tankard

#include "BlueprintExpression.h"

#include "BlueprintInspectorPort.h"

#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/UnrealType.h"
#if BI_ENGINE_AT_LEAST_5_5
#include "UObject/StrProperty.h"
#endif
#include "UObject/TextProperty.h"

namespace
{
struct FPathSegment
{
    FString Name;
    int32 Index = INDEX_NONE;
};

struct FResolvedValue
{
    const FProperty* Property = nullptr;
    const uint8* Container = nullptr;
    const UObject* Owner = nullptr;
};

struct FLiteral
{
    enum class EKind : uint8
    {
        None,
        Number,
        Boolean,
        Text
    };

    EKind Kind = EKind::None;
    double Number = 0.0;
    bool Boolean = false;
    FString Text;
};

const FProperty* FindField(const UStruct* const Struct, const FString& Name)
{
    return Struct != nullptr ? FindFProperty<FProperty>(Struct, FName(*Name)) : nullptr;
}

bool IsUnsignedIntegerField(const FProperty* const Property)
{
    return CastField<FByteProperty>(Property) != nullptr || CastField<FUInt16Property>(Property) != nullptr || CastField<FUInt32Property>(Property) != nullptr || CastField<FUInt64Property>(Property) != nullptr;
}

double ReadNumericValue(const FNumericProperty* const Numeric, const void* const Value)
{
    if (!Numeric->IsInteger())
    {
        return Numeric->GetFloatingPointPropertyValue(Value);
    }
    return IsUnsignedIntegerField(Numeric)
               ? static_cast<double>(Numeric->GetUnsignedIntPropertyValue(Value))
               : static_cast<double>(Numeric->GetSignedIntPropertyValue(Value));
}

bool SplitPath(const FString& Path, TArray<FPathSegment>& OutSegments, FString& OutError)
{
    FString Remaining = Path;
    Remaining.TrimStartAndEndInline();
    if (Remaining.IsEmpty())
    {
        OutError = TEXT("empty expression");
        return false;
    }

    int32 Position = 0;
    while (Position <= Remaining.Len())
    {
        int32 Dot = Remaining.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromStart, Position);
        if (Dot == INDEX_NONE)
        {
            Dot = Remaining.Len();
        }

        FString Part = Remaining.Mid(Position, Dot - Position);
        Part.TrimStartAndEndInline();
        if (Part.IsEmpty())
        {
            OutError = TEXT("empty path segment");
            return false;
        }

        FPathSegment Segment;
        int32 Bracket = INDEX_NONE;
        if (Part.FindChar(TEXT('['), Bracket))
        {
            const int32 Close = Part.Find(TEXT("]"), ESearchCase::CaseSensitive, ESearchDir::FromStart, Bracket);
            if (Close == INDEX_NONE || Close != Part.Len() - 1)
            {
                OutError = FString::Printf(TEXT("malformed index in '%s'"), *Part);
                return false;
            }
            const FString IndexText = Part.Mid(Bracket + 1, Close - Bracket - 1);
            Segment.Name = Part.Left(Bracket);
            Segment.Name.TrimStartAndEndInline();
            if (!IndexText.IsNumeric() || IndexText.Contains(TEXT(".")))
            {
                OutError = FString::Printf(TEXT("malformed index in '%s'"), *Part);
                return false;
            }
            const int64 ParsedIndex = FCString::Atoi64(*IndexText);
            Segment.Index = ParsedIndex >= 0 && ParsedIndex <= MAX_int32 ? static_cast<int32>(ParsedIndex) : INDEX_NONE;
            if (Segment.Name.IsEmpty() || Segment.Index < 0)
            {
                OutError = FString::Printf(TEXT("malformed index in '%s'"), *Part);
                return false;
            }
        }
        else
        {
            Segment.Name = Part;
        }

        OutSegments.Add(Segment);
        if (Dot == Remaining.Len())
        {
            break;
        }
        Position = Dot + 1;
    }

    return true;
}

bool ApplyIndex(const FProperty*& Property, const uint8*& Container, const int32 Index, FString& OutError)
{
    const FArrayProperty* const ArrayProperty = CastField<FArrayProperty>(Property);
    if (ArrayProperty == nullptr || ArrayProperty->Inner == nullptr)
    {
        OutError = FString::Printf(TEXT("'%s' is not an array"), *Property->GetName());
        return false;
    }

    FScriptArrayHelper Helper(ArrayProperty, Property->ContainerPtrToValuePtr<void>(Container));
    if (Index >= Helper.Num())
    {
        OutError = FString::Printf(TEXT("index %d is outside '%s' (%d element(s))"), Index, *Property->GetName(), Helper.Num());
        return false;
    }

    Container = static_cast<const uint8*>(Helper.GetRawPtr(Index));
    Property = ArrayProperty->Inner;
    return true;
}

bool ResolvePath(const FBlueprintExpressionContext& Context, const FString& Path, FResolvedValue& Out, FString& OutError)
{
    TArray<FPathSegment> Segments;
    if (!SplitPath(Path, Segments, OutError))
    {
        return false;
    }

    const FProperty* Property = nullptr;
    const uint8* Container = nullptr;
    const UObject* Owner = Context.Object;
    int32 Start = 0;

    if (Segments[0].Name.Equals(TEXT("This"), ESearchCase::IgnoreCase))
    {
        if (Context.Object == nullptr)
        {
            OutError = TEXT("'This' is not available");
            return false;
        }
        if (Segments[0].Index != INDEX_NONE)
        {
            OutError = TEXT("'This' cannot be indexed");
            return false;
        }

        Container = reinterpret_cast<const uint8*>(Context.Object);
        Start = 1;
        if (Segments.Num() == 1)
        {
            Out = FResolvedValue{nullptr, Container, Owner};
            return true;
        }
    }
    else
    {
        if (Context.Function != nullptr)
        {
            const FProperty* const Found = FindField(Context.Function, Segments[0].Name);
            if (Found != nullptr)
            {
                if (Context.Locals == nullptr)
                {
                    OutError = FString::Printf(TEXT("'%s' is a local, but the paused frame has no locals"), *Segments[0].Name);
                    return false;
                }
                Property = Found;
                Container = Context.Locals;
            }
        }
        // A local or parameter shadows a member of the same name: the innermost scope is what the
        // debugger should show.
        if (Property == nullptr && Context.Object != nullptr)
        {
            const FProperty* const Found = FindField(Context.Object->GetClass(), Segments[0].Name);
            if (Found != nullptr)
            {
                Property = Found;
                Container = reinterpret_cast<const uint8*>(Context.Object);
            }
        }
        if (Property == nullptr)
        {
            OutError = FString::Printf(TEXT("'%s' was not found on the paused frame"), *Segments[0].Name);
            return false;
        }
        if (Segments[0].Index != INDEX_NONE && !ApplyIndex(Property, Container, Segments[0].Index, OutError))
        {
            return false;
        }
        Start = 1;
    }

    for (int32 SegmentIndex = Start; SegmentIndex < Segments.Num(); ++SegmentIndex)
    {
        const FPathSegment& Segment = Segments[SegmentIndex];

        if (Property == nullptr)
        {
            if (Owner == nullptr)
            {
                OutError = TEXT("no object to resolve against");
                return false;
            }
            const FProperty* const Found = FindField(Owner->GetClass(), Segment.Name);
            if (Found == nullptr)
            {
                OutError = FString::Printf(TEXT("'%s' was not found on %s"), *Segment.Name, *Owner->GetName());
                return false;
            }
            Property = Found;
            Container = reinterpret_cast<const uint8*>(Owner);
        }
        else if (const FStructProperty* const StructProperty = CastField<FStructProperty>(Property))
        {
            if (StructProperty->Struct == nullptr)
            {
                OutError = FString::Printf(TEXT("'%s' has no struct type"), *Property->GetName());
                return false;
            }
            const FProperty* const Found = FindField(StructProperty->Struct, Segment.Name);
            if (Found == nullptr)
            {
                OutError = FString::Printf(TEXT("'%s' was not found on %s"), *Segment.Name, *StructProperty->Struct->GetName());
                return false;
            }
            Container = Property->ContainerPtrToValuePtr<uint8>(Container);
            Property = Found;
        }
        else if (const FObjectPropertyBase* const ObjectProperty = CastField<FObjectPropertyBase>(Property))
        {
            const UObject* const Object = ObjectProperty->GetObjectPropertyValue(Property->ContainerPtrToValuePtr<void>(Container));
            if (Object == nullptr)
            {
                OutError = FString::Printf(TEXT("'%s' is null"), *Property->GetName());
                return false;
            }
            const FProperty* const Found = FindField(Object->GetClass(), Segment.Name);
            if (Found == nullptr)
            {
                OutError = FString::Printf(TEXT("'%s' was not found on %s"), *Segment.Name, *Object->GetName());
                return false;
            }
            Container = reinterpret_cast<const uint8*>(Object);
            Property = Found;
            Owner = Object;
        }
        else
        {
            OutError = FString::Printf(TEXT("'%s' is not a struct or object, so '.%s' cannot resolve"), *Property->GetName(), *Segment.Name);
            return false;
        }

        if (Segment.Index != INDEX_NONE && !ApplyIndex(Property, Container, Segment.Index, OutError))
        {
            return false;
        }
    }

    Out = FResolvedValue{Property, Container, Owner};
    return true;
}

bool DescribeValue(const FResolvedValue& Resolved, FBlueprintExpressionValue& OutValue, FString& OutError)
{
    if (Resolved.Property == nullptr)
    {
        if (Resolved.Owner == nullptr)
        {
            OutError = TEXT("no value");
            return false;
        }
        OutValue.bValid = true;
        OutValue.Display = Resolved.Owner->GetName();
        OutValue.Type = Resolved.Owner->GetClass()->GetName();
        OutValue.ObjectValue = Resolved.Owner;
        OutValue.bIsObject = true;
        return true;
    }

    const void* const Value = Resolved.Property->ContainerPtrToValuePtr<void>(Resolved.Container);
    FString Text;
#if BI_ENGINE_AT_LEAST_5_4
    Resolved.Property->ExportTextItem_Direct(Text, Value, nullptr, const_cast<UObject*>(Resolved.Owner), PPF_PropertyWindow | PPF_BlueprintDebugView, nullptr);
#else
    Resolved.Property->ExportTextItem(Text, Value, nullptr, const_cast<UObject*>(Resolved.Owner), PPF_PropertyWindow | PPF_BlueprintDebugView, nullptr);
#endif

    OutValue.bValid = true;
    OutValue.Display = Text;
    OutValue.Type = Resolved.Property->GetCPPType();

    if (const FBoolProperty* const BoolProperty = CastField<FBoolProperty>(Resolved.Property))
    {
        OutValue.bIsBool = true;
        OutValue.bBoolValue = BoolProperty->GetPropertyValue(Value);
    }
    else if (const FNumericProperty* const NumericProperty = CastField<FNumericProperty>(Resolved.Property))
    {
        OutValue.bIsNumeric = true;
        OutValue.Number = ReadNumericValue(NumericProperty, Value);
    }
    else if (const FEnumProperty* const EnumProperty = CastField<FEnumProperty>(Resolved.Property))
    {
        const FNumericProperty* const Underlying = EnumProperty->GetUnderlyingProperty();
        if (Underlying != nullptr)
        {
            OutValue.bIsNumeric = true;
            OutValue.Number = ReadNumericValue(Underlying, Value);
        }
    }
    else if (const FStrProperty* const StringProperty = CastField<FStrProperty>(Resolved.Property))
    {
        OutValue.bIsText = true;
        OutValue.Text = StringProperty->GetPropertyValue(Value);
    }
    else if (const FNameProperty* const NameProperty = CastField<FNameProperty>(Resolved.Property))
    {
        OutValue.bIsText = true;
        OutValue.Text = NameProperty->GetPropertyValue(Value).ToString();
    }
    else if (const FTextProperty* const TextProperty = CastField<FTextProperty>(Resolved.Property))
    {
        OutValue.bIsText = true;
        OutValue.Text = TextProperty->GetPropertyValue(Value).ToString();
    }
    else if (const FObjectPropertyBase* const ObjectProperty = CastField<FObjectPropertyBase>(Resolved.Property))
    {
        OutValue.bIsObject = true;
        OutValue.ObjectValue = ObjectProperty->GetObjectPropertyValue(Value);
    }

    return true;
}

bool EvaluatePathValue(const FBlueprintExpressionContext& Context, const FString& Expression, FBlueprintExpressionValue& OutValue, FString& OutError)
{
    FResolvedValue Resolved;
    if (!ResolvePath(Context, Expression, Resolved, OutError))
    {
        return false;
    }
    return DescribeValue(Resolved, OutValue, OutError);
}

bool ParseLiteral(const FString& InText, FLiteral& Out)
{
    FString Text = InText;
    Text.TrimStartAndEndInline();
    if (Text.IsEmpty())
    {
        return false;
    }

    if (Text.Len() >= 2 && ((Text.StartsWith(TEXT("\"")) && Text.EndsWith(TEXT("\""))) || (Text.StartsWith(TEXT("'")) && Text.EndsWith(TEXT("'")))))
    {
        Out.Kind = FLiteral::EKind::Text;
        Out.Text = Text.Mid(1, Text.Len() - 2);
        return true;
    }
    if (Text.Equals(TEXT("true"), ESearchCase::IgnoreCase))
    {
        Out.Kind = FLiteral::EKind::Boolean;
        Out.Boolean = true;
        return true;
    }
    if (Text.Equals(TEXT("false"), ESearchCase::IgnoreCase))
    {
        Out.Kind = FLiteral::EKind::Boolean;
        Out.Boolean = false;
        return true;
    }
    if (Text.Equals(TEXT("none"), ESearchCase::IgnoreCase) || Text.Equals(TEXT("null"), ESearchCase::IgnoreCase))
    {
        Out.Kind = FLiteral::EKind::None;
        return true;
    }
    if (Text.IsNumeric())
    {
        Out.Kind = FLiteral::EKind::Number;
        Out.Number = FCString::Atod(*Text);
        return true;
    }
    return false;
}

void LiteralToValue(const FLiteral& Literal, FBlueprintExpressionValue& Out)
{
    Out.bValid = true;
    switch (Literal.Kind)
    {
    case FLiteral::EKind::Number:
        Out.bIsNumeric = true;
        Out.Number = Literal.Number;
        Out.Type = TEXT("number");
        break;
    case FLiteral::EKind::Boolean:
        Out.bIsBool = true;
        Out.bBoolValue = Literal.Boolean;
        Out.Type = TEXT("bool");
        break;
    case FLiteral::EKind::Text:
        Out.bIsText = true;
        Out.Text = Literal.Text;
        Out.Type = TEXT("string");
        break;
    case FLiteral::EKind::None:
        Out.bIsObject = true;
        Out.ObjectValue = nullptr;
        Out.Type = TEXT("object");
        break;
    }
}

bool ValueIsTruthy(const FBlueprintExpressionValue& Value, bool& bOutTruthy, FString& OutError)
{
    if (Value.bIsBool)
    {
        bOutTruthy = Value.bBoolValue;
        return true;
    }
    if (Value.bIsNumeric)
    {
        bOutTruthy = Value.Number != 0.0;
        return true;
    }
    if (Value.bIsText)
    {
        bOutTruthy = !Value.Text.IsEmpty();
        return true;
    }
    if (Value.bIsObject)
    {
        bOutTruthy = Value.ObjectValue != nullptr;
        return true;
    }
    OutError = TEXT("the value is not a bool, number, string or object");
    return false;
}

bool CompareValues(const FBlueprintExpressionValue& Left, const FBlueprintExpressionValue& Right, const FString& Operator, bool& bOutResult, FString& OutError)
{
    if (Operator != TEXT("==") && Operator != TEXT("!=") && Operator != TEXT("<") && Operator != TEXT("<=") && Operator != TEXT(">") && Operator != TEXT(">="))
    {
        OutError = FString::Printf(TEXT("'%s' is not a supported comparison operator"), *Operator);
        return false;
    }

    if (Left.bIsNumeric && Right.bIsNumeric)
    {
        if (Operator == TEXT("=="))
        {
            bOutResult = Left.Number == Right.Number;
        }
        else if (Operator == TEXT("!="))
        {
            bOutResult = Left.Number != Right.Number;
        }
        else if (Operator == TEXT("<"))
        {
            bOutResult = Left.Number < Right.Number;
        }
        else if (Operator == TEXT("<="))
        {
            bOutResult = Left.Number <= Right.Number;
        }
        else if (Operator == TEXT(">"))
        {
            bOutResult = Left.Number > Right.Number;
        }
        else if (Operator == TEXT(">="))
        {
            bOutResult = Left.Number >= Right.Number;
        }
        return true;
    }

    if (Left.bIsBool && Right.bIsBool && (Operator == TEXT("==") || Operator == TEXT("!=")))
    {
        bOutResult = Operator == TEXT("==") ? Left.bBoolValue == Right.bBoolValue : Left.bBoolValue != Right.bBoolValue;
        return true;
    }

    if (Left.bIsText && Right.bIsText)
    {
        const int32 Order = Left.Text.Compare(Right.Text, ESearchCase::CaseSensitive);
        if (Operator == TEXT("=="))
        {
            bOutResult = Order == 0;
        }
        else if (Operator == TEXT("!="))
        {
            bOutResult = Order != 0;
        }
        else if (Operator == TEXT("<"))
        {
            bOutResult = Order < 0;
        }
        else if (Operator == TEXT("<="))
        {
            bOutResult = Order <= 0;
        }
        else if (Operator == TEXT(">"))
        {
            bOutResult = Order > 0;
        }
        else if (Operator == TEXT(">="))
        {
            bOutResult = Order >= 0;
        }
        return true;
    }

    if (Left.bIsObject && Right.bIsObject && (Operator == TEXT("==") || Operator == TEXT("!=")))
    {
        bOutResult = Operator == TEXT("==") ? Left.ObjectValue == Right.ObjectValue : Left.ObjectValue != Right.ObjectValue;
        return true;
    }

    OutError = TEXT("the two sides have different types");
    return false;
}

bool FindComparison(const FString& InText, FString& OutLeft, FString& OutOperator, FString& OutRight)
{
    FString Text = InText;
    Text.TrimStartAndEndInline();

    bool bInString = false;
    TCHAR Quote = 0;
    for (int32 Index = 0; Index < Text.Len(); ++Index)
    {
        const TCHAR Character = Text[Index];
        if (bInString)
        {
            if (Character == Quote)
            {
                bInString = false;
            }
            continue;
        }
        if (Character == TEXT('"') || Character == TEXT('\''))
        {
            bInString = true;
            Quote = Character;
            continue;
        }

        if (Index + 1 < Text.Len())
        {
            const TCHAR Next = Text[Index + 1];
            if ((Character == TEXT('=') && Next == TEXT('=')) || (Character == TEXT('!') && Next == TEXT('=')) || (Character == TEXT('<') && Next == TEXT('=')) || (Character == TEXT('>') && Next == TEXT('=')))
            {
                const TCHAR OperatorChars[] = {Character, Next, TEXT('\0')};
                OutOperator = OperatorChars;
                OutLeft = Text.Left(Index);
                OutRight = Text.Mid(Index + 2);
                return true;
            }
        }
        if (Character == TEXT('<') || Character == TEXT('>'))
        {
            OutOperator = FString(1, &Character);
            OutLeft = Text.Left(Index);
            OutRight = Text.Mid(Index + 1);
            return true;
        }
    }
    return false;
}
} // namespace

namespace BlueprintExpression
{
bool EvaluateValue(const FBlueprintExpressionContext& Context, const FString& Expression, FBlueprintExpressionValue& OutValue, FString& OutError)
{
    // A caller may reuse the out value; a failed evaluation must not leave the previous kind flags set.
    OutValue = FBlueprintExpressionValue();
    OutError.Reset();

    FString Text = Expression;
    Text.TrimStartAndEndInline();
    if (Text.IsEmpty())
    {
        OutError = TEXT("empty expression");
        return false;
    }

    FString Left;
    FString Operator;
    FString Right;
    if (Text.StartsWith(TEXT("!")) || FindComparison(Text, Left, Operator, Right))
    {
        bool bResult = false;
        if (!EvaluateCondition(Context, Text, bResult, OutError))
        {
            return false;
        }
        OutValue.bValid = true;
        OutValue.Display = bResult ? TEXT("true") : TEXT("false");
        OutValue.Type = TEXT("bool");
        OutValue.bBoolValue = bResult;
        OutValue.bIsBool = true;
        return true;
    }

    FLiteral Literal;
    if (ParseLiteral(Text, Literal))
    {
        LiteralToValue(Literal, OutValue);
        OutValue.Display = Text;
        return true;
    }

    return EvaluatePathValue(Context, Text, OutValue, OutError);
}

bool EvaluateCondition(const FBlueprintExpressionContext& Context, const FString& Expression, bool& bOutResult, FString& OutError)
{
    bOutResult = false;
    OutError.Reset();

    FString Text = Expression;
    Text.TrimStartAndEndInline();
    if (Text.IsEmpty())
    {
        bOutResult = true;
        return true;
    }

    bool bNegate = false;
    if (Text.StartsWith(TEXT("!")))
    {
        bNegate = true;
        Text = Text.Mid(1);
        Text.TrimStartAndEndInline();
    }

    FString Left;
    FString Operator;
    FString Right;
    bool bResult = false;
    if (FindComparison(Text, Left, Operator, Right))
    {
        Left.TrimStartAndEndInline();
        Right.TrimStartAndEndInline();

        FBlueprintExpressionValue LeftValue;
        FLiteral LeftLiteral;
        if (ParseLiteral(Left, LeftLiteral))
        {
            LiteralToValue(LeftLiteral, LeftValue);
        }
        else if (!EvaluatePathValue(Context, Left, LeftValue, OutError))
        {
            return false;
        }

        FLiteral Literal;
        FBlueprintExpressionValue RightValue;
        if (ParseLiteral(Right, Literal))
        {
            LiteralToValue(Literal, RightValue);
        }
        else if (!EvaluatePathValue(Context, Right, RightValue, OutError))
        {
            return false;
        }

        if (!CompareValues(LeftValue, RightValue, Operator, bResult, OutError))
        {
            return false;
        }
    }
    else
    {
        FBlueprintExpressionValue Value;
        FLiteral Literal;
        if (ParseLiteral(Text, Literal))
        {
            LiteralToValue(Literal, Value);
        }
        else if (!EvaluatePathValue(Context, Text, Value, OutError))
        {
            return false;
        }
        if (!ValueIsTruthy(Value, bResult, OutError))
        {
            return false;
        }
    }

    bOutResult = bNegate ? !bResult : bResult;
    return true;
}

FString FormatLogMessage(const FBlueprintExpressionContext& Context, const FString& Message)
{
    FString Result;
    Result.Reserve(Message.Len() + 32);
    for (int32 Index = 0; Index < Message.Len(); ++Index)
    {
        const TCHAR Character = Message[Index];

        if ((Character == TEXT('{') || Character == TEXT('}')) && Index + 1 < Message.Len() && Message[Index + 1] == Character)
        {
            Result.AppendChar(Character);
            ++Index;
            continue;
        }

        if (Character != TEXT('{'))
        {
            Result.AppendChar(Character);
            continue;
        }

        int32 Close = INDEX_NONE;
        int32 Depth = 1;
        TCHAR Quote = 0;
        for (int32 Scan = Index + 1; Scan < Message.Len(); ++Scan)
        {
            const TCHAR Scanned = Message[Scan];
            if (Quote != 0)
            {
                if (Scanned == TEXT('\\') && Scan + 1 < Message.Len())
                {
                    ++Scan;
                }
                else if (Scanned == Quote)
                {
                    Quote = 0;
                }
                continue;
            }
            if (Scanned == TEXT('"') || Scanned == TEXT('\''))
            {
                Quote = Scanned;
                continue;
            }
            if (Scanned == TEXT('{'))
            {
                ++Depth;
                continue;
            }
            if (Scanned == TEXT('}') && --Depth == 0)
            {
                Close = Scan;
                break;
            }
        }
        if (Close == INDEX_NONE)
        {
            Result.AppendChar(Character);
            continue;
        }

        const FString Inner = Message.Mid(Index + 1, Close - Index - 1);
        FBlueprintExpressionValue Value;
        FString Error;
        if (EvaluateValue(Context, Inner, Value, Error))
        {
            Result += Value.Display;
        }
        else
        {
            Result += FString::Printf(TEXT("<%s>"), *Error);
        }
        Index = Close;
    }
    return Result;
}

bool EvaluateHitCondition(const FString& Condition, const int64 HitCount, bool& bOutShouldBreak)
{
    FString Text = Condition;
    Text.TrimStartAndEndInline();
    bOutShouldBreak = false;
    if (Text.IsEmpty())
    {
        bOutShouldBreak = true;
        return true;
    }

    FString Operator = TEXT("==");
    if (Text.StartsWith(TEXT(">=")))
    {
        Operator = TEXT(">=");
        Text = Text.Mid(2);
    }
    else if (Text.StartsWith(TEXT("<=")))
    {
        Operator = TEXT("<=");
        Text = Text.Mid(2);
    }
    else if (Text.StartsWith(TEXT("==")))
    {
        Text = Text.Mid(2);
    }
    else if (Text.StartsWith(TEXT(">")))
    {
        Operator = TEXT(">");
        Text = Text.Mid(1);
    }
    else if (Text.StartsWith(TEXT("<")))
    {
        Operator = TEXT("<");
        Text = Text.Mid(1);
    }
    else if (Text.StartsWith(TEXT("%")))
    {
        Operator = TEXT("%");
        Text = Text.Mid(1);
    }

    Text.TrimStartAndEndInline();
    if (!Text.IsNumeric() || Text.Contains(TEXT(".")))
    {
        return false;
    }

    const int64 Target = FCString::Atoi64(*Text);
    if (Operator == TEXT("%") && Target <= 0)
    {
        return false;
    }
    if (Operator == TEXT(">="))
    {
        bOutShouldBreak = HitCount >= Target;
    }
    else if (Operator == TEXT("<="))
    {
        bOutShouldBreak = HitCount <= Target;
    }
    else if (Operator == TEXT(">"))
    {
        bOutShouldBreak = HitCount > Target;
    }
    else if (Operator == TEXT("<"))
    {
        bOutShouldBreak = HitCount < Target;
    }
    else if (Operator == TEXT("%"))
    {
        bOutShouldBreak = HitCount % Target == 0;
    }
    else
    {
        bOutShouldBreak = HitCount == Target;
    }
    return true;
}
} // namespace BlueprintExpression
