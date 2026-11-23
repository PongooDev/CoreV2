#pragma once

#include "Containers/Array.h"

#include <cstdint>

class FString;

struct FBase64
{
    // The bytes Source decodes to (standard alphabet, '=' padded), or 0 when its length isn't a multiple of four.
    static int32_t GetDecodedDataSize(const FString& Source);
    static bool Decode(const FString& Source, TArray<uint8_t>& OutDest);
};
