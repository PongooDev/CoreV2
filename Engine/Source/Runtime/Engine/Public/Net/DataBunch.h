#pragma once

#include "ObjectLayout.h"

#include <cstdint>

class UChannel;

class FOutBunch
{
public:
    FOutBunch(UChannel* InChannel, bool bClose)
    {
        Natives::FOutBunch_FOutBunch(this, InChannel, bClose);
    }

    ~FOutBunch()
    {
        CallVirtual<void>(this, 0, 0u);
    }

    FOutBunch(const FOutBunch&) = delete;
    FOutBunch& operator=(const FOutBunch&) = delete;

    uint8_t& bReliable() { return Field<uint8_t>(0xE8); }
    uint8_t& bDormant() { return Field<uint8_t>(0xE6); }

private:
    template <typename T>
    T& Field(size_t Offset)
    {
        return *reinterpret_cast<T*>(Storage + Offset);
    }

    alignas(8) uint8_t Storage[0x110];
};

static_assert(sizeof(FOutBunch) == 0x110);
