#include "pch.h"
#include "BackendRequest.h"
#include "Configuration.h"
#include "Log.h"

#include <winhttp.h>

#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <mutex>
#include <thread>

#pragma comment(lib, "winhttp.lib")

namespace
{
    struct FRequest
    {
        std::wstring What;
        std::wstring Path;
        std::string Body;
        std::wstring ClientId;
        std::wstring Secret;
        // Set for a GET, which answers through it; a POST's answer is only logged.
        std::function<void(uint32_t Status, const std::string& Body)> OnAnswer;
    };

    struct FResponse
    {
        uint32_t Status = 0;
        std::string Body;
    };

    // One worker sends the requests in the order they were made.
    std::mutex QueueMutex;
    std::condition_variable QueueReady;
    std::deque<FRequest> Queue;
    bool bWorkerStarted = false;

    // The worker's own.
    HINTERNET Session = nullptr;
    std::string Token;
    std::wstring TokenClient;
    std::chrono::steady_clock::time_point TokenExpires;

    std::wstring Widen(const std::string& Text)
    {
        return std::wstring(Text.begin(), Text.end());
    }

    std::string Narrow(const std::wstring& Text)
    {
        std::string Result;
        for (const wchar_t Character : Text)
        {
            Result.push_back(Character < 0x80 ? static_cast<char>(Character) : '?');
        }
        return Result;
    }

    std::string Base64(const std::string& Data)
    {
        constexpr const char* Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string Result;
        for (size_t Index = 0; Index < Data.size(); Index += 3)
        {
            const size_t Count = std::min<size_t>(3, Data.size() - Index);
            uint32_t Bits = 0;
            for (size_t Byte = 0; Byte < 3; ++Byte)
            {
                Bits = (Bits << 8) | (Byte < Count ? static_cast<uint8_t>(Data[Index + Byte]) : 0);
            }
            for (size_t Sextet = 0; Sextet < 4; ++Sextet)
            {
                Result.push_back(Sextet <= Count ? Alphabet[(Bits >> (18 - 6 * Sextet)) & 0x3F] : '=');
            }
        }
        return Result;
    }

    FResponse Exchange(const wchar_t* Verb, const std::wstring& Path, const std::wstring& Headers, const std::string& Body)
    {
        FResponse Response;
        const std::wstring Url = Configuration::Get().Backend + Path;
        wchar_t Host[256] = {};
        wchar_t UrlPath[2048] = {};
        wchar_t Extra[2048] = {};
        URL_COMPONENTS Parts{};
        Parts.dwStructSize = sizeof(Parts);
        Parts.lpszHostName = Host;
        Parts.dwHostNameLength = static_cast<DWORD>(std::size(Host));
        Parts.lpszUrlPath = UrlPath;
        Parts.dwUrlPathLength = static_cast<DWORD>(std::size(UrlPath));
        Parts.lpszExtraInfo = Extra;
        Parts.dwExtraInfoLength = static_cast<DWORD>(std::size(Extra));
        if (!WinHttpCrackUrl(Url.c_str(), 0, 0, &Parts))
        {
            Log::Error(L"Backend: {} is not a URL", Url);
            return Response;
        }

        if (!Session)
        {
            Session = WinHttpOpen(L"Core V2", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (Session)
            {
                WinHttpSetTimeouts(Session, 5000, 5000, 10000, 10000);
            }
        }
        HINTERNET Connection = Session ? WinHttpConnect(Session, Host, Parts.nPort, 0) : nullptr;
        const bool bSecure = Parts.nScheme == INTERNET_SCHEME_HTTPS;
        const std::wstring Target = std::wstring(UrlPath) + Extra;
        HINTERNET Request = Connection ? WinHttpOpenRequest(Connection, Verb, Target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, bSecure ? WINHTTP_FLAG_SECURE : 0) : nullptr;
        if (Request && bSecure)
        {
            // The backend's own certificate is self-signed.
            DWORD Flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA | SECURITY_FLAG_IGNORE_CERT_CN_INVALID | SECURITY_FLAG_IGNORE_CERT_DATE_INVALID | SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
            WinHttpSetOption(Request, WINHTTP_OPTION_SECURITY_FLAGS, &Flags, sizeof(Flags));
        }

        void* const Data = const_cast<char*>(Body.data());
        const DWORD Length = static_cast<DWORD>(Body.size());
        if (Request && WinHttpSendRequest(Request, Headers.c_str(), static_cast<DWORD>(-1), Data, Length, Length, 0) && WinHttpReceiveResponse(Request, nullptr))
        {
            DWORD Status = 0;
            DWORD Size = sizeof(Status);
            WinHttpQueryHeaders(Request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &Status, &Size, WINHTTP_NO_HEADER_INDEX);
            Response.Status = Status;
            DWORD Available = 0;
            while (WinHttpQueryDataAvailable(Request, &Available) && Available > 0)
            {
                std::string Chunk(Available, '\0');
                DWORD Read = 0;
                if (!WinHttpReadData(Request, Chunk.data(), Available, &Read) || Read == 0)
                {
                    break;
                }
                Response.Body.append(Chunk.data(), Read);
            }
        }
        else
        {
            Log::Warning(L"Backend: no answer from {} (WinHTTP error {})", Url, GetLastError());
        }

        if (Request)
        {
            WinHttpCloseHandle(Request);
        }
        if (Connection)
        {
            WinHttpCloseHandle(Connection);
        }
        return Response;
    }

    bool HasToken(const FRequest& Request)
    {
        if (!Token.empty() && TokenClient == Request.ClientId && std::chrono::steady_clock::now() < TokenExpires)
        {
            return true;
        }

        Token.clear();
        const std::wstring Headers = L"Content-Type: application/x-www-form-urlencoded\r\nAuthorization: basic " + Widen(Base64(Narrow(Request.ClientId + L":" + Request.Secret)));
        const FResponse Response = Exchange(L"POST", L"/account/api/oauth/token", Headers, "grant_type=client_credentials");
        Token = Response.Status == 200 ? BackendRequest::JsonField(Response.Body, "access_token") : std::string();
        if (Token.empty())
        {
            Log::Warning(L"Backend: no token for client {} (HTTP {})", Request.ClientId, Response.Status);
            return false;
        }
        // A minute early, so it never runs out on the way.
        const int32_t ExpiresIn = std::atoi(BackendRequest::JsonField(Response.Body, "expires_in").c_str());
        TokenClient = Request.ClientId;
        TokenExpires = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(ExpiresIn - 60, 0));
        return true;
    }

    void Send(const FRequest& Request)
    {
        for (int32_t Attempt = 0; Attempt < 2; ++Attempt)
        {
            if (!HasToken(Request))
            {
                break;
            }
            const bool bGet = static_cast<bool>(Request.OnAnswer);
            const std::wstring Headers = std::wstring(bGet ? L"Accept: application/json" : L"Content-Type: application/json") + L"\r\nAuthorization: bearer " + Widen(Token);
            const FResponse Response = Exchange(bGet ? L"GET" : L"POST", Request.Path, Headers, Request.Body);
            if (Response.Status == 401)
            {
                // The backend restarted with its tokens gone, or revoked this one: once more with a new one.
                Token.clear();
                continue;
            }
            if (bGet)
            {
                if (Response.Status != 200)
                {
                    Log::Warning(L"Backend: {} failed (HTTP {}): {}", Request.What, Response.Status, Widen(Response.Body.substr(0, 300)));
                }
                Request.OnAnswer(Response.Status, Response.Body);
                return;
            }
            if (Response.Status >= 200 && Response.Status < 300)
            {
                Log::Info(L"Backend: {} sent (HTTP {})", Request.What, Response.Status);
            }
            else
            {
                Log::Warning(L"Backend: {} failed (HTTP {}): {}", Request.What, Response.Status, Widen(Response.Body.substr(0, 300)));
            }
            return;
        }
        Log::Warning(L"Backend: {} not sent", Request.What);
        if (Request.OnAnswer)
        {
            Request.OnAnswer(0, std::string());
        }
    }

    void Work()
    {
        for (;;)
        {
            FRequest Request;
            {
                std::unique_lock Lock(QueueMutex);
                QueueReady.wait(Lock, [] { return !Queue.empty(); });
                Request = std::move(Queue.front());
                Queue.pop_front();
            }
            Send(Request);
        }
    }

    void Enqueue(FRequest Request)
    {
        {
            std::scoped_lock Lock(QueueMutex);
            Queue.push_back(std::move(Request));
            if (!bWorkerStarted)
            {
                bWorkerStarted = true;
                std::thread(Work).detach();
            }
        }
        QueueReady.notify_one();
    }
}

void BackendRequest::PostJson(std::wstring What, std::wstring Path, std::string Body, std::wstring ClientId, std::wstring Secret)
{
    Enqueue(FRequest{ std::move(What), std::move(Path), std::move(Body), std::move(ClientId), std::move(Secret), nullptr });
}

void BackendRequest::GetJson(std::wstring What, std::wstring Path, std::wstring ClientId, std::wstring Secret, std::function<void(uint32_t Status, const std::string& Body)> OnAnswer)
{
    Enqueue(FRequest{ std::move(What), std::move(Path), std::string(), std::move(ClientId), std::move(Secret), std::move(OnAnswer) });
}

std::string BackendRequest::JsonField(const std::string& Json, const std::string& Name)
{
    const size_t Key = Json.find("\"" + Name + "\"");
    size_t Start = Key == std::string::npos ? std::string::npos : Json.find(':', Key);
    if (Start == std::string::npos)
    {
        return {};
    }
    Start = Json.find_first_not_of(" \t\r\n", Start + 1);
    if (Start == std::string::npos)
    {
        return {};
    }
    if (Json[Start] == '"')
    {
        const size_t End = Json.find('"', Start + 1);
        return End == std::string::npos ? std::string() : Json.substr(Start + 1, End - Start - 1);
    }
    const size_t End = Json.find_first_of(",} \t\r\n", Start);
    return Json.substr(Start, End == std::string::npos ? std::string::npos : End - Start);
}

