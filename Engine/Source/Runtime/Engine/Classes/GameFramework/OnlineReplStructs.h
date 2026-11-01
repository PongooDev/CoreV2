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
// their size, and only read in place. Core never makes or copies one: a 0x18 copy of a 1.11 id leaves its
// ReplicationBytes unset (#175). Parameters are copied through the script struct (Reflection::FParams::CopyStruct,
// UScriptStruct::CopyScriptStruct), which copies the build's whole struct.
struct FUniqueNetIdRepl : public FUniqueNetIdWrapper
{
    DefineUnrealStruct(FUniqueNetIdRepl, Engine)

public:
    FUniqueNetIdRepl() = delete;
    FUniqueNetIdRepl(const FUniqueNetIdRepl&) = delete;
    FUniqueNetIdRepl& operator=(const FUniqueNetIdRepl&) = delete;
};

static_assert(sizeof(FUniqueNetIdRepl) == 0x18);
