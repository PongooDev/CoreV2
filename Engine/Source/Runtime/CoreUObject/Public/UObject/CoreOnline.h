#pragma once

#include "Containers/UnrealString.h"
#include "ObjectLayout.h"

class FUniqueNetId
{
public:
    bool IsValid() const
    {
        // Never call slot -1: without the slot, an id is valid when it prints as something (the engine's string ids
        // are valid exactly when not empty), and invalid when ToString is unresolved too.
        if (Natives::FUniqueNetId_IsValidIndex < 0)
        {
            return !ToString().IsEmpty();
        }
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
