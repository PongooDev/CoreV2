#pragma once

#include "Containers/UnrealString.h"
#include "ObjectLayout.h"

class FUniqueNetId
{
public:
    bool IsValid() const
    {
        return CallVirtual<bool>(const_cast<FUniqueNetId*>(this), Natives::FUniqueNetId_IsValidIndex);
    }

    FString ToString() const
    {
        FString Result;
        if (Natives::FUniqueNetId_ToStringIndex >= 0)
        {
            CallVirtual<FString*>(const_cast<FUniqueNetId*>(this), Natives::FUniqueNetId_ToStringIndex, &Result);
        }
        return Result;
    }
};
