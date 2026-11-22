// Copyright (c) 2024-2026 Paul Tankard

#pragma once

#include "CoreMinimal.h"

class FProperty;
class UObject;

/**
 * The debugger's text-based variable writer.
 *
 * Supports values whose text form cannot mutate containers: numeric, bool, enum, name, string, text,
 * object/soft/class references, and structs made only of those. Interface references, delegates, and
 * array/set/map properties are rejected: their text form can add or remove elements, which is not the
 * single-property edit this writer validates.
 *
 * Struct values without a custom text import are validated member by member before the import, so an
 * out-of-range or undeclared member value is rejected with the member's name instead of being wrapped
 * by the engine. Such a struct is editable only when every one of its members is; one holding a map or
 * a delegate is rejected as a whole, and the message names the member that caused it. A struct that
 * provides ImportTextItem is imported as a single opaque value, so its members are never inspected.
 *
 * Empty text clears a name/string/text property (there is no separate clear operation).
 *
 * Object references are imported with the property window flags, so ImportText may resolve a path
 * with FindObject/LoadObject and run that object's PostLoad on the calling thread. Apply must
 * therefore be called on the game thread, which is where the subsystem marshals every write.
 */
namespace BlueprintInspectorVariableEdit
{
/** Why a property cannot be edited as text. None means it can. */
enum class ETextEditRejection : uint8
{
    None,
    /** The property pointer is null. */
    NoProperty,
    /** The caller is not on the game thread, where the property's metadata and storage are valid. */
    OffGameThread,
    /** A const parameter cannot be written through its stack slot. */
    ConstParameter,
    /** A fixed-size array reports a per-element size; writing the whole array would overflow. */
    FixedSizeArray,
    /** The type's text form can mutate containers, or the type is not supported at all. */
    UnsupportedType,
    /** The text is not a valid value for the property: parse failure, out of range, wrong shape. */
    InvalidValue,
};

/**
 * The reason Apply would reject the property, and a message naming the property or the first offending
 * struct member. Off the game thread this reports OffGameThread rather than reporting the type as
 * unsupported, so a threading bug is not disguised as a property-type rejection.
 *
 * This covers type and storage only. A value-dependent rejection (`InvalidValue` for text the import
 * cannot read) comes back from Apply, which takes the text; a None here is not a promise that Apply
 * will accept a particular value.
 *
 * Game thread only, unless you only need the OffGameThread answer.
 */
ETextEditRejection GetTextEditRejection(const FProperty* Property, FString& OutError);

/** True when GetTextEditRejection would return None. Game thread only, like that function. */
bool IsTextEditable(const FProperty* Property);

/** As above, and fills OutError with the reason when it returns false. Game thread only. */
bool IsTextEditable(const FProperty* Property, FString& OutError);

#if WITH_DEV_AUTOMATION_TESTS
/** True while the scratch import in Apply holds its garbage-collection guard. */
bool IsImportGcGuardHeldForTest();
#endif

struct FEnumValueAcceptance
{
    bool bBitflags = false;
    bool bDeclaredEnumeratorsReadable = false;
    bool bParsed = false;
    bool bDeclared = false;
    bool bGeneratedMax = false;
};

/**
 * Whether a parsed numeric enum value may be written. The caller must already have parsed the text and
 * checked it against the underlying type's range. Bitflags combine entries by design, and a build
 * without editor-only metadata cannot see the declared enumerators; in both cases the number is accepted
 * without a declared-enumerator check.
 */
bool AcceptsNumericEnumValue(const FEnumValueAcceptance& Value);

/**
 * Imports Text into the property's storage and re-exports the stored value. The import runs against
 * scratch storage first, so a parse failure or trailing garbage leaves the live property untouched.
 *
 * Game thread only (object-reference imports can load assets).
 *
 * @param Property   May be null; returns false when null.
 * @param Container  Base address of the storage that contains the property (the object base, or the
 *                   element/struct address for a nested value), NOT a pointer to the value; the
 *                   property offset is applied internally. May be null; returns false when null.
 * @param Owner      Import context; may be null.
 * @param Text       The full text form to import.
 *
 * On failure the live property is unchanged, OutValue is empty, and OutError describes the failure.
 * On success OutValue holds the re-exported stored value and OutError is empty.
 */
bool Apply(const FProperty* Property, uint8* Container, UObject* Owner, const FString& Text, FString& OutValue, FString& OutError);
} // namespace BlueprintInspectorVariableEdit
