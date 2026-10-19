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
