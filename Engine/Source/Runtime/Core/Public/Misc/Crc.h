#pragma once

#include <cstdint>

struct FCrc
{
    // The old MSB-first CRC-32 (polynomial 0x04C11DB7) that UE 4.2x still hashes an FGuid with.
    static uint32_t MemCrc_DEPRECATED(const void* Data, int32_t Length, uint32_t CRC = 0);
};
