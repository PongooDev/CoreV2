#include "pch.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Hooks.h"
#include "McpServiceConfig.h"
#include "Natives.h"

namespace
{
    constexpr const wchar_t* DedicatedServerClientId = L"313dfed3ec3023e52d3be9dda178bdfa";
    constexpr const wchar_t* DedicatedServerClientSecret = L"111aa434a6780d54f5cf759149639f20";

    using FUpdateServicePermissions = void (*)(void*, void*, FMcpServiceConfig*, void*);
    FUpdateServicePermissions UpdateServicePermissionsOG = nullptr;

    void UpdateServicePermissions(void* Environment, void* Context, FMcpServiceConfig* ServiceConfig, void* Extra)
    {
        UpdateServicePermissionsOG(Environment, Context, ServiceConfig, Extra);

#if WITH_SERVER_CODE
        if (!ServiceConfig->FindServicePermissions(DedicatedServerServicePermissionsName))
        {
            ServiceConfig->ServicePermissions.Add(FMcpServicePermissions{ DedicatedServerServicePermissionsName, DedicatedServerClientId, DedicatedServerClientSecret });
        }
#endif
    }
}

void FFortMcpConfig::Hook()
{
    Hooks::Detour(L"FortMcpConfig service permissions", reinterpret_cast<void*>(Natives::FortMcpConfig_UpdateServicePermissions), &UpdateServicePermissions, &UpdateServicePermissionsOG);
}
