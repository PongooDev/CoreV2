#pragma once

#include "Interfaces/IHttpRequest.h"
#include "McpServiceConfig.h"
#include "ObjectLayout.h"
#include "OnlineSubsystemImpl.h"
#include "Templates/SharedPointer.h"

class FUniqueNetId;

class FGameServiceMcp
{
public:
    DefineNativeObject(FMcpServiceConfig, ServiceConfig, FGameServiceMcp_ServiceConfig)
};

typedef TSharedPtr<FGameServiceMcp, ESPMode::ThreadSafe> FGameServiceMcpPtr;

class FOnlineSubsystemMcp : public FOnlineSubsystemImpl
{
public:
    DefineNativeMember(FGameServiceMcpPtr, McpGameService, FOnlineSubsystemMcp_McpGameService)

    FGameServiceMcpPtr GetMcpGameService() const
    {
        return McpGameService;
    }

    bool QueueRequest(FGameServiceMcp& Service, const FUniqueNetId& UserId, const TSharedRef<IHttpRequest>& HttpRequest, FString& OutError);
    bool QueueRequest(FGameServiceMcp& Service, const FString& ClientId, const TSharedRef<IHttpRequest>& HttpRequest, FString& OutError);
};
