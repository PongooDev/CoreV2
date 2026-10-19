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
        CallVirtual<void, UMcpProfile, const TSharedRef<IHttpRequest>&, uint32_t>(this, Natives::UMcpProfile_PrepareRequestIndex, HttpRequest, ResponseType);
    }
};
