#include "pch.h"
#include "Misc/Crc.h"

#include <array>

namespace
{
    constexpr std::array<uint32_t, 256> CRCTable_DEPRECATED = []
    {
        std::array<uint32_t, 256> Table{};
        for (uint32_t Index = 0; Index < 256; ++Index)
        {
            uint32_t CRC = Index << 24;
            for (int32_t Bit = 0; Bit < 8; ++Bit)
            {
                CRC = (CRC & 0x80000000u) ? (CRC << 1) ^ 0x04C11DB7u : CRC << 1;
            }
            Table[Index] = CRC;
        }
        return Table;
    }();
}

uint32_t FCrc::MemCrc_DEPRECATED(const void* Data, int32_t Length, uint32_t CRC)
{
    const uint8_t* Bytes = static_cast<const uint8_t*>(Data);
    CRC = ~CRC;
    for (int32_t Index = 0; Index < Length; ++Index)
    {
        CRC = (CRC << 8) ^ CRCTable_DEPRECATED[(CRC >> 24) ^ Bytes[Index]];
    }
    return ~CRC;
}
