#pragma once

#include <cstdint>

template <class TEnum>
class TEnumAsByte
{
public:
    typedef TEnum EnumType;

    TEnumAsByte() = default;

    TEnumAsByte(TEnum InValue)
        : Value(static_cast<uint8_t>(InValue))
    {
    }

    explicit TEnumAsByte(int32_t InValue)
        : Value(static_cast<uint8_t>(InValue))
    {
    }

    bool operator==(TEnum InValue) const { return static_cast<TEnum>(Value) == InValue; }
    bool operator==(TEnumAsByte InValue) const { return Value == InValue.Value; }
    operator TEnum() const { return static_cast<TEnum>(Value); }
    TEnum GetValue() const { return static_cast<TEnum>(Value); }

private:
    uint8_t Value;
};
