#pragma once

#include <cstdint>

class FNetworkGUID
{
public:
    uint32_t Value = 0;

    FNetworkGUID() = default;

    explicit FNetworkGUID(uint32_t V)
        : Value(V)
    {
    }

    bool IsValid() const
    {
        return Value > 0;
    }

    bool IsDynamic() const
    {
        return Value > 0 && !(Value & 1);
    }

    bool IsStatic() const
    {
        return Value & 1;
    }

    bool operator==(const FNetworkGUID& Other) const = default;

    friend uint32_t GetTypeHash(const FNetworkGUID& Guid)
    {
        return Guid.Value;
    }
};

static_assert(sizeof(FNetworkGUID) == 4);
