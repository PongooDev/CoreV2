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

    using FCreateWebSocket = void* (*)(void* Module, void* OutWebSocket, const FString& Url, const void* Protocols);
    FCreateWebSocket CreateWebSocketOG = nullptr;

    void* CreateWebSocketHK(void* Module, void* OutWebSocket, const FString& Url, const void* Protocols)
    {
        std::wstring Target = RedirectedURL(Url.ToStdString());
        if (Target.empty())
        {
            return CreateWebSocketOG(Module, OutWebSocket, Url, Protocols);
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
        return CreateWebSocketOG(Module, OutWebSocket, FString(Target), Protocols);
    }

    // FWebSocketsModule::CreateWebSocket: the call in FStompClient's constructor (the function besides
    // FStompModule::StartupModule that names the v12.stomp subprotocol) to `mov rcx, [rcx+8]; mov rax, [rcx];
    // call [rax+10h]` (the module's WebSocketsManager->CreateWebSocket).
    uintptr_t FindCreateWebSocket()
    {
        static constexpr uint8_t Body[] = { 0x48, 0x8B, 0x49, 0x08, 0x48, 0x8B, 0xDA, 0x48, 0x8B, 0x01, 0xFF, 0x50, 0x10 };
        for (const uintptr_t Ref : Finder::StringRefs(L"v12.stomp"))
        {
            const uintptr_t Function = Finder::FunctionStart(Ref);
            const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
            for (const auto& [Call, Target] : End ? Finder::DirectCalls(Function, End) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
            {
                if (Finder::IsReadable(reinterpret_cast<const void*>(Target + 6), sizeof(Body)) && std::memcmp(reinterpret_cast<const void*>(Target + 6), Body, sizeof(Body)) == 0)
                {
                    return Target;
                }
            }
        }
        return 0;
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
    if (const uintptr_t CreateWebSocket = FindCreateWebSocket())
    {
        Hooks::Detour(L"FWebSocketsModule::CreateWebSocket", reinterpret_cast<void*>(CreateWebSocket), &CreateWebSocketHK, &CreateWebSocketOG);
    }
    else
    {
        Log::Error(L"Redirect: FWebSocketsModule::CreateWebSocket not found, STOMP notifications stay on Epic's host");
    }
    return bCurl && bWinInet;
}
