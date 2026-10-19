#pragma once

#include "ObjectLayout.h"

class FUniqueNetId
{
public:
    bool IsValid() const
    {
        return CallVirtual<bool>(const_cast<FUniqueNetId*>(this), Natives::FUniqueNetId_IsValidIndex);
    }
};
