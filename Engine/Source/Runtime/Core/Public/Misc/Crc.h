#pragma once

#include <cstdint>

struct FCrc
{
    // The old MSB-first CRC-32 (polynomial 0x04C11DB7) that UE 4.2x still hashes an FGuid with.
    static uint32_t MemCrc_DEPRECATED(const void* Data, int32_t Length, uint32_t CRC = 0);
    // GetTypeHash(FString)'s: each character upper-cased (a to z only, as 9.10 CL-6573057 0x3A5AB0 does) and fed in
    // low byte then high byte, through the same table.
    static uint32_t Strihash_DEPRECATED(const wchar_t* Data);
};
