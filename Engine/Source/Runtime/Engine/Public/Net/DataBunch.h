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

    bool IsError() const { return (Field<uint8_t>(0x28) & 0x40) != 0; }
    int64_t GetNumBits() const { return Field<int64_t>(0x98); }

    uint8_t GetClose() const { return Field<uint8_t>(0xE5); }
    void SetClose(uint8_t Value) { Field<uint8_t>(0xE5) = Value; }
    uint8_t GetDormant() const { return Field<uint8_t>(0xE6); }
    void SetDormant(uint8_t Value) { Field<uint8_t>(0xE6) = Value; }
    uint8_t GetReliable() const { return Field<uint8_t>(0xE8); }
    void SetReliable(uint8_t Value) { Field<uint8_t>(0xE8) = Value; }

    __declspec(property(get = GetClose, put = SetClose)) uint8_t bClose;
    __declspec(property(get = GetDormant, put = SetDormant)) uint8_t bDormant;
    __declspec(property(get = GetReliable, put = SetReliable)) uint8_t bReliable;

private:
    template <typename T>
    T& Field(size_t Offset) const
    {
        return *reinterpret_cast<T*>(const_cast<uint8_t*>(Storage) + Offset);
    }

    alignas(8) uint8_t Storage[0x110];
};

static_assert(sizeof(FOutBunch) == 0x110);
