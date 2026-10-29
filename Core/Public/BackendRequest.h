#pragma once

#include <cstdint>
#include <functional>
#include <string>

// Requests Core makes to Core-Backend (-CoreBackend) itself, for what the game has no request of its own for. They go
// through WinHTTP on a thread of their own, so the game thread never waits on the backend.
namespace BackendRequest
{
    // POSTs a JSON body to Path with a client_credentials token of the OAuth client ClientId:Secret (fetched when first
    // needed and kept until it expires). The answer is only logged, with What naming the request.
    void PostJson(std::wstring What, std::wstring Path, std::string Body, std::wstring ClientId, std::wstring Secret);

    // GETs Path the same way and hands OnAnswer the HTTP status and body, on the requests' thread (0 and nothing when
    // the backend didn't answer or gave no token). The game's objects belong to the game thread: GameThread::Enqueue.
    void GetJson(std::wstring What, std::wstring Path, std::wstring ClientId, std::wstring Secret, std::function<void(uint32_t Status, const std::string& Body)> OnAnswer);

    // POSTs Body the same way and hands OnAnswer the answer, as GetJson does.
    void PostJson(std::wstring What, std::wstring Path, std::string Body, std::wstring ClientId, std::wstring Secret, std::function<void(uint32_t Status, const std::string& Body)> OnAnswer);

    // The value of a top-level "Name": "..." or "Name": number/true/false/null in a JSON object, as text (a string's
    // without its quotes), or "" when it has none.
    std::string JsonField(const std::string& Json, const std::string& Name);
}
