#pragma once

#include "Containers/UnrealString.h"

#include <cstdint>

// Epic's JSON value types (Json module, Dom/JsonValue.h), as the engine lays them out: Core only reads values the engine
// made (an MCP profile's stats, UMcpProfile::GetStat), never builds or destroys one itself; the engine's shared
// reference does that.
enum class EJson : int32_t
{
    None,
    Null,
    String,
    Number,
    Boolean,
    Array,
    Object
};

class FJsonValue
{
public:
    EJson GetType() const { return Type; }

    // FJsonValue::TryGetString for a string value (FJsonValueString's own), which is all Core reads. Epic's is virtual
    // and converts numbers and booleans too; Core doesn't call the engine's virtuals.
    bool TryGetString(FString& OutString) const;

protected:
    // The engine's vtable, then the type (FJsonValue's only member).
    void* VTable;
    EJson Type;
};

class FJsonValueString : public FJsonValue
{
public:
    const FString& GetValue() const { return Value; }

protected:
    FString Value;
};

static_assert(sizeof(FJsonValue) == 0x10);
static_assert(sizeof(FJsonValueString) == 0x20);

inline bool FJsonValue::TryGetString(FString& OutString) const
{
    if (Type != EJson::String)
    {
        return false;
    }
    OutString = static_cast<const FJsonValueString*>(this)->GetValue();
    return true;
}
