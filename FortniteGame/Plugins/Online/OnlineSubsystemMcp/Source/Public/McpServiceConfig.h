#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "ObjectLayout.h"

inline constexpr const wchar_t* DedicatedServerServicePermissionsName = L"DedicatedServer";

struct FMcpServicePermissions
{
    FString Name;
    FString Id;
    FString Key;
};

static_assert(sizeof(FMcpServicePermissions) == 0x30);

struct FMcpServiceConfig
{
    DefineNativeMember(TArray<FMcpServicePermissions>, ServicePermissions, FMcpServiceConfig_ServicePermissions)

    const FMcpServicePermissions* FindServicePermissions(const FString& Name) const;
};
