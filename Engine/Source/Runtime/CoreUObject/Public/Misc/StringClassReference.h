#pragma once

#include "Misc/StringAssetReference.h"

struct FStringClassReference : public FStringAssetReference
{
    FStringClassReference() = default;

    FStringClassReference(const FString& PathString)
        : FStringAssetReference(PathString)
    {
    }
};
static_assert(sizeof(FStringClassReference) == 0x10);

template <>
inline constexpr size_t Reflection::SoftPathGrowth<FStringClassReference> = 8;
