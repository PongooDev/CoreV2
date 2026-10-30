#include "pch.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Hooks.h"
#include "McpServiceConfig.h"
#include "Natives.h"

namespace
{
    using FUpdateServicePermissions = void (*)(void*, void*, FMcpServiceConfig*, void*);
    FUpdateServicePermissions UpdateServicePermissionsOG = nullptr;

    void UpdateServicePermissions(void* Environment, void* Context, FMcpServiceConfig* ServiceConfig, void* Extra)
    {
        UpdateServicePermissionsOG(Environment, Context, ServiceConfig, Extra);

#if WITH_SERVER_CODE
        FFortMcpConfig::AddDedicatedServerPermissions(*ServiceConfig);
#endif
    }
}

void FFortMcpConfig::AddDedicatedServerPermissions(FMcpServiceConfig& ServiceConfig)
{
    if (!ServiceConfig.FindServicePermissions(DedicatedServerServicePermissionsName))
    {
        ServiceConfig.ServicePermissions.Add(FMcpServicePermissions{ DedicatedServerServicePermissionsName, DedicatedServerClientId, DedicatedServerClientSecret });
    }
}

void FFortMcpConfig::Hook()
{
    Hooks::Detour(L"FortMcpConfig service permissions", reinterpret_cast<void*>(Natives::FortMcpConfig_UpdateServicePermissions), &UpdateServicePermissions, &UpdateServicePermissionsOG);
}
