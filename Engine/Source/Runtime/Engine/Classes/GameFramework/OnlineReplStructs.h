#pragma once

#include "Reflection.h"
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

// UE 4.20's FUniqueNetIdRepl (1.11) adds ReplicationBytes, a TArray<uint8>, after the wrapper: 0x28 bytes, not 0x18.
// Core declares 4.19's, so the engine's members of this type are found by their struct (DefineUStructProperty), not
// their size, and only read in place.
struct FUniqueNetIdRepl : public FUniqueNetIdWrapper
{
    DefineUnrealStruct(FUniqueNetIdRepl, Engine)
};

static_assert(sizeof(FUniqueNetIdRepl) == 0x18);
