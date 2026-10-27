#pragma once

#include "Interfaces/IHttpRequest.h"
#include "ObjectLayout.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"

class UMcpProfile : public UObject
{
    DefineUnrealClass(UMcpProfile, McpProfileSys)

public:
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
