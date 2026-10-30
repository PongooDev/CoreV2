#include "pch.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "McpServiceConfig.h"
#include "Natives.h"
#include "OnlineSubsystemImpl.h"
#include "OnlineSubsystemMcp.h"

#include <algorithm>
#include <atomic>
#include <format>
#include <string>
#include <string_view>
#include <vector>

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

    // Search bounds, not layout: the subsystem keeps its services as TSharedPtr members within its first MaxServiceMember
    // bytes (1.9.1: +0x1C8 to +0x3C8), and a service object has its FBaseServiceMcp part within its first MaxServiceBase
    // bytes (FOnlineIdentityMcp's is behind IOnlineIdentity, 1.9.1 +0x2B0).
    constexpr uintptr_t MaxServiceMember = 0x800;
    constexpr uintptr_t MaxServiceBase = 0x400;
    constexpr int32_t MaxVTableSlots = 128;

    // The end of the readable memory Address is in, or 0.
    uintptr_t ReadableEnd(uintptr_t Address)
    {
        MEMORY_BASIC_INFORMATION Info = {};
        if (!Address || !VirtualQuery(reinterpret_cast<const void*>(Address), &Info, sizeof(Info)) || Info.State != MEM_COMMIT
            || (Info.Protect & (PAGE_NOACCESS | PAGE_GUARD)) || !(Info.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        {
            return 0;
        }
        return reinterpret_cast<uintptr_t>(Info.BaseAddress) + Info.RegionSize;
    }

    bool IsInImage(uintptr_t Address)
    {
        static const uintptr_t Begin = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        static const uintptr_t End = Begin + reinterpret_cast<const IMAGE_NT_HEADERS64*>(Begin + reinterpret_cast<const IMAGE_DOS_HEADER*>(Begin)->e_lfanew)->OptionalHeader.SizeOfImage;
        return Address >= Begin && Address < End;
    }

    // What every service's FBaseServiceMcp part has, read off the game service: GetBasicAuth in a vtable slot, and a
    // pointer back to its subsystem.
    struct FServiceShape
    {
        uintptr_t Subsystem = 0;
        int32_t BasicAuthSlot = -1;
        uintptr_t SubsystemMember = 0;
    };

    // Whether the FBaseServiceMcp part of a service of Shape's subsystem starts at Base, readable up to End.
    bool IsService(uintptr_t Base, uintptr_t End, const FServiceShape& Shape)
    {
        if (Base + static_cast<uintptr_t>(Natives::FGameServiceMcp_ServiceConfig) > End || *reinterpret_cast<const uintptr_t*>(Base + Shape.SubsystemMember) != Shape.Subsystem)
        {
            return false;
        }
        const uintptr_t VTable = *reinterpret_cast<const uintptr_t*>(Base);
        const uintptr_t Slot = VTable + Shape.BasicAuthSlot * sizeof(void*);
        return IsInImage(VTable) && IsInImage(Slot + sizeof(void*) - 1) && *reinterpret_cast<void* const*>(Slot) == Natives::FBaseServiceMcp_GetBasicAuth;
    }

    bool ReadShape(const FOnlineSubsystemMcp* Subsystem, FServiceShape& Shape)
    {
        Shape.Subsystem = reinterpret_cast<uintptr_t>(Subsystem);
        const uintptr_t GameServiceMember = Shape.Subsystem + Natives::FOnlineSubsystemMcp_McpGameService;
        if (ReadableEnd(Shape.Subsystem) < GameServiceMember + sizeof(void*))
        {
            return false;
        }

        const uintptr_t GameService = reinterpret_cast<uintptr_t>(Subsystem->McpGameService.Get());
        const uintptr_t End = ReadableEnd(GameService);
        if (GameService + static_cast<uintptr_t>(Natives::FGameServiceMcp_ServiceConfig) > End)
        {
            return false;
        }
        const uintptr_t VTable = *reinterpret_cast<const uintptr_t*>(GameService);
        for (int32_t Slot = 0; Slot < MaxVTableSlots && Shape.BasicAuthSlot < 0 && IsInImage(VTable + (Slot + 1) * sizeof(void*) - 1); ++Slot)
        {
            Shape.BasicAuthSlot = reinterpret_cast<void* const*>(VTable)[Slot] == Natives::FBaseServiceMcp_GetBasicAuth ? Slot : -1;
        }
        for (uintptr_t Member = sizeof(void*); Member < static_cast<uintptr_t>(Natives::FGameServiceMcp_ServiceConfig) && !Shape.SubsystemMember; Member += sizeof(void*))
        {
            Shape.SubsystemMember = *reinterpret_cast<const uintptr_t*>(GameService + Member) == Shape.Subsystem ? Member : 0;
        }
        return Shape.BasicAuthSlot >= 0 && Shape.SubsystemMember;
    }

    // The service's ini section (FBaseServiceMcp::IniSectionName, "OnlineSubsystemMcp.GameServiceMcp" and so on), the
    // first such string after its config, for the log only.
    std::wstring ServiceName(uintptr_t Base, uintptr_t End)
    {
        constexpr std::wstring_view Prefix = L"OnlineSubsystemMcp.";
        const uintptr_t Config = Base + Natives::FGameServiceMcp_ServiceConfig;
        for (uintptr_t At = Config + Natives::FMcpServiceConfig_ServicePermissions + sizeof(FString); At + sizeof(FString) <= End && At < Config + MaxServiceBase; At += sizeof(void*))
        {
            const auto* Text = *reinterpret_cast<const wchar_t* const*>(At);
            const int32_t Num = *reinterpret_cast<const int32_t*>(At + sizeof(void*));
            const int32_t Max = *reinterpret_cast<const int32_t*>(At + sizeof(void*) + sizeof(int32_t));
            const uintptr_t TextEnd = reinterpret_cast<uintptr_t>(Text) + Num * sizeof(wchar_t);
            if (Num > static_cast<int32_t>(Prefix.size()) + 1 && Num <= 128 && Max >= Num && ReadableEnd(reinterpret_cast<uintptr_t>(Text)) >= TextEnd
                && !Text[Num - 1] && std::wstring_view(Text, Prefix.size()) == Prefix)
            {
                return std::wstring(Text + Prefix.size());
            }
        }
        return std::format(L"a service at 0x{:X}", Base);
    }
}

void FFortMcpConfig::AddDedicatedServerPermissions(FMcpServiceConfig& ServiceConfig)
{
    if (!ServiceConfig.FindServicePermissions(DedicatedServerServicePermissionsName))
    {
        ServiceConfig.ServicePermissions.Add(FMcpServicePermissions{ DedicatedServerServicePermissionsName, DedicatedServerClientId, DedicatedServerClientSecret });
    }
}

int32_t FFortMcpConfig::AddDedicatedServerPermissionsToServices(const FOnlineSubsystemMcp* Subsystem)
{
#if WITH_SERVER_CODE
    FServiceShape Shape;
    if (!Subsystem || !Natives::FBaseServiceMcp_GetBasicAuth || !ReadShape(Subsystem, Shape))
    {
        return 0;
    }

    // Each service is a TSharedPtr member of the subsystem: the object, and its reference controller, which for a
    // MakeShared object has the object inside it. The same service turns up through both and is counted once.
    std::vector<uintptr_t> Services;
    std::wstring Added;
    std::wstring Had;
    const uintptr_t SubsystemEnd = ReadableEnd(Shape.Subsystem);
    for (uintptr_t Member = Shape.Subsystem + sizeof(void*); Member + sizeof(void*) <= SubsystemEnd && Member < Shape.Subsystem + MaxServiceMember; Member += sizeof(void*))
    {
        const uintptr_t Object = *reinterpret_cast<const uintptr_t*>(Member);
        const uintptr_t End = Object % sizeof(void*) == 0 && !IsInImage(Object) ? ReadableEnd(Object) : 0;
        for (uintptr_t Base = Object; Base < End && Base < Object + MaxServiceBase; Base += sizeof(void*))
        {
            if (!IsService(Base, End, Shape) || std::find(Services.begin(), Services.end(), Base) != Services.end())
            {
                continue;
            }
            Services.push_back(Base);

            FMcpServiceConfig& Config = *reinterpret_cast<FMcpServiceConfig*>(Base + Natives::FGameServiceMcp_ServiceConfig);
            const bool bHad = Config.FindServicePermissions(DedicatedServerServicePermissionsName) != nullptr;
            AddDedicatedServerPermissions(Config);
            std::wstring& List = bHad ? Had : Added;
            List += (List.empty() ? L"" : L", ") + ServiceName(Base, End);
        }
    }

    Log::Info(L"FortMcpConfig: the server's \"{}\" permission is now in {} MCP services' configs (added to: {}; already there: {})",
        DedicatedServerServicePermissionsName, Services.size(), Added.empty() ? L"none" : Added, Had.empty() ? L"none" : Had);
    return static_cast<int32_t>(Services.size());
#else
    return 0;
#endif
}

void FFortMcpConfig::AddDedicatedServerPermissionsToLoadedServices()
{
    if (!Finder::ResolveMcpBasicAuth())
    {
        Log::Error(L"FortMcpConfig: the MCP services can't be told apart, so the server's permission goes in only as its profile requests need it");
        return;
    }

    int32_t Services = 0;
    for (const std::atomic<const FOnlineSubsystemImpl*>& Slot : FOnlineSubsystemImpl::Subsystems)
    {
        if (const FOnlineSubsystemImpl* Subsystem = Slot.load())
        {
            Services += AddDedicatedServerPermissionsToServices(static_cast<const FOnlineSubsystemMcp*>(Subsystem));
        }
    }
    if (!Services)
    {
        Log::Error(L"FortMcpConfig: no MCP subsystem has started yet, so the server's permission goes in only as its profile requests need it");
    }
}

void FFortMcpConfig::Hook()
{
    Hooks::Detour(L"FortMcpConfig service permissions", reinterpret_cast<void*>(Natives::FortMcpConfig_UpdateServicePermissions), &UpdateServicePermissions, &UpdateServicePermissionsOG);
}
