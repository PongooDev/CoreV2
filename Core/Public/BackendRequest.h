#pragma once

#include <string>

// Requests Core makes to Core-Backend (-CoreBackend) itself, for what the game has no request of its own for. They go
// through WinHTTP on a thread of their own, so the game thread never waits on the backend.
namespace BackendRequest
{
    // POSTs a JSON body to Path with a client_credentials token of the OAuth client ClientId:Secret (fetched when first
    // needed and kept until it expires). The answer is only logged, with What naming the request.
    void PostJson(std::wstring What, std::wstring Path, std::string Body, std::wstring ClientId, std::wstring Secret);
}
