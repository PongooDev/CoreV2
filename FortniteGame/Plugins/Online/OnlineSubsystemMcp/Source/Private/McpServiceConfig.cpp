#include "pch.h"
#include "McpServiceConfig.h"

#include <cwchar>

const FMcpServicePermissions* FMcpServiceConfig::FindServicePermissions(const FString& Name) const
{
    const TArray<FMcpServicePermissions>& Permissions = ServicePermissions;
    for (const FMcpServicePermissions& Permission : Permissions)
    {
        if (_wcsicmp(*Permission.Name, *Name) == 0)
        {
            return &Permission;
        }
    }
    return nullptr;
}
