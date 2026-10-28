#pragma once

#include "Interfaces/IHttpRequest.h"
#include "ObjectLayout.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"

class UMcpProfile : public UObject
{
    DefineUnrealClass(UMcpProfile, McpProfileSys)

public:
    // The revision of the profile the client holds: -1 until a query's answer has filled it.
    DefineUProperty(int64_t, ProfileRevision)

    // Queries the whole profile (another account's through the public URL), as the game does when it registers a player.
    bool QueryProfile()
    {
        if (!Natives::UMcpProfile_QueryProfile)
        {
            return false;
        }
        // An unbound FMcpQueryComplete: QueryProfile copies it into the request's context and calls nothing back. 0x38
        // bytes in 1.8.1 (inline storage, its heap pointer, then the size, which is 0 while unbound).
        alignas(16) uint8_t OnComplete[0x80] = {};
        Natives::UMcpProfile_QueryProfile(this, OnComplete);
        return true;
    }

    void PrepareRequest(const TSharedRef<IHttpRequest>& HttpRequest, uint32_t ResponseType)
    {
        if (Natives::UMcpProfile_PrepareRequestTakesObject)
        {
            // 1.8.1 (0x3C514E0) / 1.8.2 (0x3C5AB70): PrepareRequest(IHttpRequest&, uint32), the request object itself.
            CallVirtual<void, UMcpProfile, IHttpRequest&, uint32_t>(this, Natives::UMcpProfile_PrepareRequestIndex, *HttpRequest, ResponseType);
            return;
        }
        CallVirtual<void, UMcpProfile, const TSharedRef<IHttpRequest>&, uint32_t>(this, Natives::UMcpProfile_PrepareRequestIndex, HttpRequest, ResponseType);
    }
};
