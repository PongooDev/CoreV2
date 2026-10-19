#pragma once

#include "Templates/SharedPointer.h"
#include "UObject/NameTypes.h"

struct FKeyDetails;

struct FKey
{
    FName KeyName;
    TSharedPtr<FKeyDetails> KeyDetails;
};

static_assert(sizeof(FKey) == 0x18);
