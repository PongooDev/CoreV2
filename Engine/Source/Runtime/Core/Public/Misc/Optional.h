#pragma once

#include <new>
#include <utility>

// UE 4.23's TOptional layout (Misc/Optional.h): the value's bytes, then bIsSet. Only what Core reads off the game's
// optionals and builds for them.
template <typename OptionalType>
struct TOptional
{
    TOptional() = default;

    TOptional(const OptionalType& InValue)
    {
        new (&Value) OptionalType(InValue);
        bIsSet = true;
    }

    TOptional(const TOptional&) = delete;
    TOptional& operator=(const TOptional&) = delete;

    ~TOptional()
    {
        if (bIsSet)
        {
            GetValue().~OptionalType();
        }
    }

    bool IsSet() const
    {
        return bIsSet;
    }

    const OptionalType& GetValue() const
    {
        return *reinterpret_cast<const OptionalType*>(&Value);
    }

    OptionalType& GetValue()
    {
        return *reinterpret_cast<OptionalType*>(&Value);
    }

private:
    alignas(OptionalType) unsigned char Value[sizeof(OptionalType)] = {};
    bool bIsSet = false;
};
