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

    DefineNativeBitfield(ArIsError, FArchive_ArIsError, Natives::FArchive_ArIsErrorMask)
    DefineNativeMember(int64_t, Num, FBitWriter_Num)
    DefineNativeBitfield(bClose, FOutBunch_bClose, Natives::FOutBunch_bCloseMask)
    DefineNativeBitfield(bDormant, FOutBunch_bDormant, Natives::FOutBunch_bDormantMask)
    DefineNativeBitfield(bReliable, FOutBunch_bReliable, Natives::FOutBunch_bReliableMask)

    bool IsError() const { return ArIsError; }
    int64_t GetNumBits() const { return Num; }

    static constexpr size_t StorageSize = 0x200;

private:
    alignas(16) uint8_t Storage[StorageSize];
};
