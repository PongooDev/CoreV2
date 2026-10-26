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
}

bool HttpRedirect::Hook()
{
    Log::Info(L"Redirect: Epic hosts go to {}", Configuration::Get().Backend);

    const bool bCurl = HookRequest(L"FCurlHttpRequest::SetURL",
        L"Could not set libcurl options for easy handle, processing HTTP request failed. Increase verbosity for additional information.",
        &CurlSetURLOG, &SetURL<&CurlSetURLOG>);
    const bool bWinInet = HookRequest(L"FHttpRequestWinInet::SetURL",
        L"ProcessRequest failed. Could not initialize Internet connection.",
        &WinInetSetURLOG, &SetURL<&WinInetSetURLOG>);
    // The client's STOMP notifications (StompClientMcp) connect through libwebsockets, which doesn't use the HTTP proxy,
    // to the service config's URL, read before the hotfix can change it (Backend-Evidence "STOMP").
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
