#pragma once

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Misc/Guid.h"
#include "UObject/NameTypes.h"

#include <cstdint>

// The keys of the encrypted cosmetic paks (5.21+): FortniteGame.FortKeyChain, which FFortContentEncryptionManager::
// MergeInKeyChain takes. 6.10 (CL-4480234) and later hold the keys in an array; 5.21 to 6.02 in a map by pak GUID.

enum class EFortEncryptionStatus : uint8_t
{
    ENCRYPTED,
    RELEASED,
};

struct FFortEncryptionKey
{
    FName Name;
    FGuid Guid;
    TArray<uint8_t> Bytes;
    EFortEncryptionStatus Status = EFortEncryptionStatus::ENCRYPTED;
};

static_assert(sizeof(FFortEncryptionKey) == 0x30);

struct FFortKeyChain
{
    uint32_t Hash = 0;
    TArray<FFortEncryptionKey> Keys;
};

static_assert(sizeof(FFortKeyChain) == 0x18);

// 5.21 to 6.02.
struct FFortEncryptionKeyBytes
{
    TArray<uint8_t> Bytes;
};

struct FFortKeyChainByGuid
{
    uint32_t Hash = 0;
    TMap<FGuid, FFortEncryptionKeyBytes> Keys;
};

static_assert(sizeof(FFortKeyChainByGuid) == 0x58);
