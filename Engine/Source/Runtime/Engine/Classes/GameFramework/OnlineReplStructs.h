#pragma once

#include "Templates/SharedPointer.h"
#include "UObject/CoreOnline.h"

struct FUniqueNetIdWrapper
{
    virtual ~FUniqueNetIdWrapper() = default;

    bool IsValid() const
    {
        return UniqueNetId.IsValid() && UniqueNetId->IsValid();
    }

    const FUniqueNetId& operator*() const
    {
        return *UniqueNetId;
    }

    FString ToString() const
    {
        return IsValid() ? UniqueNetId->ToString() : FString(L"INVALID");
    }

protected:
    TSharedPtr<const FUniqueNetId> UniqueNetId;
};

struct FUniqueNetIdRepl : public FUniqueNetIdWrapper
{
};

static_assert(sizeof(FUniqueNetIdRepl) == 0x18);
