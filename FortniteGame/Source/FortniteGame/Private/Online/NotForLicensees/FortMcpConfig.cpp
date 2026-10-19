#include "pch.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Hooks.h"
#include "McpServiceConfig.h"
#include "Natives.h"

#include <cwchar>

namespace
{
    constexpr const wchar_t* DedicatedServerPermissionName = L"DedicatedServer";
    constexpr const wchar_t* DedicatedServerClientId = L"313dfed3ec3023e52d3be9dda178bdfa";
    constexpr const wchar_t* DedicatedServerClientSecret = L"111aa434a6780d54f5cf759149639f20";

    using FUpdateServicePermissions = void (*)(void*, void*, uint8_t*, void*);
    FUpdateServicePermissions UpdateServicePermissionsOG = nullptr;

    void UpdateServicePermissions(void* Environment, void* Context, uint8_t* ServiceConfig, void* Extra)
    {
        UpdateServicePermissionsOG(Environment, Context, ServiceConfig, Extra);

#if WITH_SERVER_CODE
        TArray<FMcpServicePermissions>& Permissions = *reinterpret_cast<TArray<FMcpServicePermissions>*>(ServiceConfig + Natives::FMcpServiceConfig_ServicePermissions);
        for (const FMcpServicePermissions& Permission : Permissions)
        {
            if (_wcsicmp(*Permission.Name, DedicatedServerPermissionName) == 0)
            {
                return;
            }
        }

        Permissions.Add(FMcpServicePermissions{ DedicatedServerPermissionName, DedicatedServerClientId, DedicatedServerClientSecret });
#endif
    }
}

void FFortMcpConfig::Hook()
{
    Hooks::Detour(L"FortMcpConfig service permissions", reinterpret_cast<void*>(Natives::FortMcpConfig_UpdateServicePermissions), &UpdateServicePermissions, &UpdateServicePermissionsOG);
}
