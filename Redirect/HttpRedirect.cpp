#include "pch.h"
#include "HttpRedirect.h"
#include "Configuration.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Interfaces/IHttpRequest.h"

#include <algorithm>
#include <cstring>
#include <cwctype>
#include <string>
#include <string_view>

namespace
{
    constexpr int32_t SetURLSlot = 10;
    constexpr int32_t SetContentSlot = 11;
    constexpr int32_t ProcessRequestSlot = 15;
    constexpr int32_t RequestSlots = 23;

    using FSetURL = void (*)(IHttpRequest*, const FString&);

    FSetURL CurlSetURLOG = nullptr;
    FSetURL WinInetSetURLOG = nullptr;

    bool IsMemberAssign(const void* Function)
    {
        const auto* Code = static_cast<const uint8_t*>(Function);
        return Code[0] == 0x48 && Code[2] == 0xC1 && ((Code[1] == 0x83 && Code[4] == 0xE9) || (Code[1] == 0x81 && Code[7] == 0xE9));
    }

    void** RequestVTable(const wchar_t* ProcessRequestText)
    {
        const std::vector<uintptr_t> Refs = Finder::StringRefs(ProcessRequestText);
        void** VTable = Refs.size() == 1 ? Finder::VTableContaining(Finder::FunctionStart(Refs.front()), ProcessRequestSlot, RequestSlots) : nullptr;
        if (VTable && !IsMemberAssign(VTable[SetContentSlot]))
        {
            Log::Error(L"Redirect: the request vtable at 0x{:X} does not have the IHttpRequest layout", reinterpret_cast<uintptr_t>(VTable));
            return nullptr;
        }
        return VTable;
    }

    bool IsEpicHost(std::wstring Host)
    {
        std::transform(Host.begin(), Host.end(), Host.begin(), [](wchar_t Character) { return static_cast<wchar_t>(std::towlower(Character)); });
        for (const std::wstring_view Domain : { L"epicgames.com", L"epicgames.net" })
        {
            if (Host == Domain || (Host.size() > Domain.size() && Host.ends_with(Domain) && Host[Host.size() - Domain.size() - 1] == L'.'))
            {
                return true;
            }
        }
        return false;
    }

    std::wstring RedirectedURL(const std::wstring& URL)
    {
        const size_t Scheme = URL.find(L"://");
        if (Scheme == std::wstring::npos)
        {
            return {};
        }

        const size_t HostStart = Scheme + 3;
        const size_t PathStart = URL.find_first_of(L"/?#", HostStart);
        std::wstring Host = URL.substr(HostStart, PathStart == std::wstring::npos ? std::wstring::npos : PathStart - HostStart);
        if (const size_t User = Host.rfind(L'@'); User != std::wstring::npos)
        {
            Host.erase(0, User + 1);
        }
        if (const size_t Port = Host.find(L':'); Port != std::wstring::npos)
        {
            Host.erase(Port);
        }

        if (!IsEpicHost(Host))
        {
            return {};
        }
        return Configuration::Get().Backend + (PathStart == std::wstring::npos ? std::wstring(L"/") : URL.substr(PathStart));
    }

    template <FSetURL* Original>
    void SetURL(IHttpRequest* Request, const FString& URL)
    {
        const std::wstring Target = RedirectedURL(URL.ToStdString());
        if (Target.empty())
        {
            (*Original)(Request, URL);
            return;
        }

        Log::Info(L"Redirect: {} -> {}", URL.ToStdString(), Target);
        (*Original)(Request, FString(Target));
    }

    bool HookRequest(const wchar_t* Name, const wchar_t* ProcessRequestText, FSetURL* Original, FSetURL Replacement)
    {
        void** VTable = RequestVTable(ProcessRequestText);
        return Hooks::SwapVTable(Name, VTable, SetURLSlot, reinterpret_cast<void*>(Replacement), reinterpret_cast<void**>(Original));
    }

    // 1.1 and 1.2: CreateWebSocket(Url, Protocols). 1.7.2: CreateWebSocket(Url, Protocol, UpgradeHeaders) and
    // CreateWebSocket(Url, Protocols, UpgradeHeaders). The fifth (stack) argument is passed through; 1.1 and 1.2 never
    // read it.
    using FCreateWebSocket = void* (*)(void* Module, void* OutWebSocket, const FString& Url, const void* Protocols, const void* UpgradeHeaders);
    FCreateWebSocket CreateWebSocketOG[2] = {};

    template <int32_t Index>
    void* CreateWebSocketHK(void* Module, void* OutWebSocket, const FString& Url, const void* Protocols, const void* UpgradeHeaders)
    {
        std::wstring Target = RedirectedURL(Url.ToStdString());
        if (Target.empty())
        {
            return CreateWebSocketOG[Index](Module, OutWebSocket, Url, Protocols, UpgradeHeaders);
        }
        if (Target.starts_with(L"https://"))
        {
            Target.replace(0, 5, L"wss");
        }
        else if (Target.starts_with(L"http://"))
        {
            Target.replace(0, 4, L"ws");
        }

        Log::Info(L"Redirect: {} -> {}", Url.ToStdString(), Target);
        return CreateWebSocketOG[Index](Module, OutWebSocket, FString(Target), Protocols, UpgradeHeaders);
    }

    // A CreateWebSocket body: mov rcx, [reg+8] (the module's WebSocketsManager), then call [reg+10h] (its
    // CreateWebSocket), within the first 0x80 bytes.
    bool ForwardsToManager(uintptr_t Function)
    {
        const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
        bool bManager = false;
        for (uintptr_t Site = Function; End && Site + 4 <= End && Site < Function + 0x80; ++Site)
        {
            const auto* Code = reinterpret_cast<const uint8_t*>(Site);
            bManager |= Code[0] == 0x48 && Code[1] == 0x8B && (Code[2] & 0xF8) == 0x48 && (Code[2] & 0x07) != 0x04 && Code[3] == 0x08;
            if (bManager && Code[0] == 0xFF && (Code[1] & 0xF8) == 0x50 && (Code[1] & 0x07) != 0x04 && Code[2] == 0x10)
            {
                return true;
            }
        }
        return false;
    }

    // FWebSocketsModule::CreateWebSocket, from the two functions that name the v12.stomp subprotocol. On 1.1 and 1.2 it
    // is non-virtual, called directly by FStompClient's constructor. On 1.7.2 the STOMP and matchmaking clients reach
    // two virtuals (slots 8 and 9) through the module's vtable, taken from FStompModule::StartupModule's (slot 1): the
    // module singleton is still null when Redirect hooks.
    std::vector<uintptr_t> FindCreateWebSockets()
    {
        std::vector<uintptr_t> Found;
        const auto Add = [&Found](uintptr_t Function)
        {
            if (std::find(Found.begin(), Found.end(), Function) == Found.end())
            {
                Found.push_back(Function);
            }
        };
        for (const uintptr_t Ref : Finder::StringRefs(L"v12.stomp"))
        {
            const uintptr_t Function = Finder::FunctionStart(Ref);
            const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
            if (void** Module = End ? Finder::VTableContaining(Function, 1, 8) : nullptr)
            {
                for (int32_t Slot = 8; Slot < 12; ++Slot)
                {
                    const uintptr_t Entry = reinterpret_cast<uintptr_t>(Module[Slot]);
                    if (Entry < Finder::TextBegin() || Entry >= Finder::TextEnd())
                    {
                        break;
                    }
                    if (ForwardsToManager(Entry))
                    {
                        Add(Entry);
                    }
                }
                continue;
            }
            for (const auto& [Call, Target] : End ? Finder::DirectCalls(Function, End) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
            {
                if (ForwardsToManager(Target))
                {
                    Add(Target);
                }
            }
        }
        return Found;
    }

    // 6.00 (CL-4395664 0x2B33390) has FLwsWebSocketsManager::InitWebSockets give libwebsockets FHttpModule's proxy
    // (lws_context_creation_info::http_proxy_address, from -httpproxy), which 5.41's (0x2ACF7F0) doesn't. Every WebSocket
    // then opens with "CONNECT host:port" to the backend the launcher names as the proxy, and the backend takes a CONNECT
    // for a TLS tunnel to an Epic https host, so the plain ws:// matchmaking connection failed on the client
    // (Core-Backend #327: "Connecting to matchmaking service", FailedToConnectToMMS) and on the server
    // (HandleWebSocketConnectionError, then AFortGameSessionDedicated::Restart reloads the map). Redirect points the
    // WebSockets at the backend itself, as on every build before 6.00, so the manager initialises with the HTTP module's
    // proxy address empty and the address goes back afterwards for libcurl.
    using FInitWebSockets = uintptr_t (*)(void* Manager, const void* Protocols);
    using FGetHttpModule = uint8_t* (*)();
    FInitWebSockets InitWebSocketsOG = nullptr;
    FGetHttpModule GetHttpModule = nullptr;
    int32_t HttpProxyAddressOffset = -1;

    uintptr_t InitWebSocketsHK(void* Manager, const void* Protocols)
    {
        // FHttpModule::HttpProxyAddress, an FString (data, count, capacity): empty while libwebsockets reads it.
        uint8_t* const ProxyAddress = GetHttpModule() + HttpProxyAddressOffset;
        uint8_t Saved[16];
        std::memcpy(Saved, ProxyAddress, sizeof(Saved));
        std::memset(ProxyAddress, 0, sizeof(Saved));
        const uintptr_t Result = InitWebSocketsOG(Manager, Protocols);
        std::memcpy(ProxyAddress, Saved, sizeof(Saved));
        Log::Info(L"Redirect: libwebsockets started without the HTTP proxy, so WebSockets go straight to their host");
        return Result;
    }

    // In InitWebSockets: the call to FHttpModule::Get, then the proxy address's test, `cmp dword ptr [rax+Count], 1; jle`,
    // and its read, `mov r64, [rax+Data]` with Count = Data + 8 (6.00 0x2B33697: call 0x1F1DE20 ... cmp [rax+50h], 1;
    // jle; mov rdx, [rax+48h]).
    void HookWebSocketsProxy()
    {
        uintptr_t Init = 0;
        for (const uintptr_t Ref : Finder::StringRefs(L"Failed to initialize libwebsockets"))
        {
            const uintptr_t Function = Finder::FunctionStart(Ref);
            Init = !Init || Init == Function ? Function : static_cast<uintptr_t>(-1);
        }
        const uintptr_t End = Init && Init != static_cast<uintptr_t>(-1) ? Finder::FunctionEnd(Init) : 0;
        uintptr_t Get = 0;
        int32_t Offset = -1;
        for (uintptr_t Site = Init; End && Site + 10 <= End && Offset < 0; ++Site)
        {
            const auto* Code = reinterpret_cast<const uint8_t*>(Site);
            if (Code[0] != 0x83 || Code[1] != 0x78 || Code[3] != 0x01 || Code[4] != 0x7E || Code[6] != 0x48 || Code[7] != 0x8B
                || (Code[8] & 0xC7) != 0x40 || Code[9] + 8 != Code[2])
            {
                continue;
            }
            for (uintptr_t Call = Site - 5; Call >= Init && Call + 0x30 > Site; --Call)
            {
                const uintptr_t Target = *reinterpret_cast<const uint8_t*>(Call) == 0xE8 ? Finder::RelativeTarget(Call, 1, 5) : 0;
                if (Target >= Finder::TextBegin() && Target < Finder::TextEnd() && Finder::FunctionStart(Target) == Target)
                {
                    Get = Target;
                    Offset = Code[9];
                    break;
                }
            }
        }
        if (!Get)
        {
            // Before 6.00 the manager never reads the HTTP proxy.
            return;
        }
        const std::vector<void**> Slots = Finder::ReadOnlySlotsHolding(Init);
        if (Slots.size() != 1)
        {
            Log::Error(L"Redirect: FLwsWebSocketsManager::InitWebSockets (0x{:X}) is in {} vtable slots, WebSockets keep the HTTP proxy", Init - Finder::ImageBase(), Slots.size());
            return;
        }
        GetHttpModule = reinterpret_cast<FGetHttpModule>(Get);
        HttpProxyAddressOffset = Offset;
        Log::Info(L"Redirect: FLwsWebSocketsManager::InitWebSockets 0x{:X} gives libwebsockets the HTTP proxy (FHttpModule::Get 0x{:X}, HttpProxyAddress +0x{:X})",
            Init - Finder::ImageBase(), Get - Finder::ImageBase(), Offset);
        Hooks::SwapVTable(L"FLwsWebSocketsManager::InitWebSockets", Slots.front(), 0, reinterpret_cast<void*>(&InitWebSocketsHK), reinterpret_cast<void**>(&InitWebSocketsOG));
    }
}

bool HttpRedirect::Hook()
{
    Log::Info(L"Redirect: Epic hosts go to {}", Configuration::Get().Backend);

    const bool bCurl = HookRequest(L"FCurlHttpRequest::SetURL",
        L"Could not set libcurl options for easy handle, processing HTTP request failed. Increase verbosity for additional information.",
        &CurlSetURLOG, &SetURL<&CurlSetURLOG>);
    // 5.41 (CL-4363240) has no WinInet requests: FHttpModule::StartupModule only warns that "-HTTP=WinInet is no longer
    // valid" (0x1EBB2F9) and makes every request libcurl's, so there is no FHttpRequestWinInet vtable to hook.
    static constexpr wchar_t WinInetRemoved[] = L"-HTTP=WinInet is no longer valid";
    const bool bWinInetBuilt = !Finder::FindReadOnly(WinInetRemoved, sizeof(WinInetRemoved));
    if (!bWinInetBuilt)
    {
        Log::Info(L"Redirect: this build has no WinInet requests (\"-HTTP=WinInet is no longer valid\"), libcurl's carry them all");
    }
    const bool bWinInet = !bWinInetBuilt || HookRequest(L"FHttpRequestWinInet::SetURL",
        L"ProcessRequest failed. Could not initialize Internet connection.",
        &WinInetSetURLOG, &SetURL<&WinInetSetURLOG>);
    // The client's STOMP notifications (StompClientMcp) connect through libwebsockets, which doesn't use the HTTP proxy
    // (from 6.00 it would, HookWebSocketsProxy), to the service config's URL, read before the hotfix can change it
    // (Backend-Evidence "STOMP").
    HookWebSocketsProxy();
    const std::vector<uintptr_t> CreateWebSockets = FindCreateWebSockets();
    if (CreateWebSockets.empty() || CreateWebSockets.size() > 2)
    {
        Log::Error(L"Redirect: FWebSocketsModule::CreateWebSocket not found ({} candidates), STOMP notifications stay on Epic's host", CreateWebSockets.size());
    }
    if (CreateWebSockets.size() >= 1 && CreateWebSockets.size() <= 2)
    {
        Hooks::Detour(L"FWebSocketsModule::CreateWebSocket", reinterpret_cast<void*>(CreateWebSockets[0]), &CreateWebSocketHK<0>, &CreateWebSocketOG[0]);
    }
    if (CreateWebSockets.size() == 2)
    {
        Hooks::Detour(L"FWebSocketsModule::CreateWebSocket (protocols)", reinterpret_cast<void*>(CreateWebSockets[1]), &CreateWebSocketHK<1>, &CreateWebSocketOG[1]);
    }
    return bCurl && bWinInet;
}
