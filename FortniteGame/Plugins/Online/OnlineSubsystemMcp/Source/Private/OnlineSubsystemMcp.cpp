#include "pch.h"
#include "OnlineSubsystemMcp.h"
#include "Natives.h"

bool FOnlineSubsystemMcp::QueueRequest(FGameServiceMcp& Service, const FUniqueNetId& UserId, const TSharedRef<IHttpRequest>& HttpRequest, FString& OutError)
{
    return Natives::FOnlineSubsystemMcp_QueueRequestForUser(this, Service, UserId, HttpRequest, OutError);
}

bool FOnlineSubsystemMcp::QueueRequest(FGameServiceMcp& Service, const FString& ClientId, const TSharedRef<IHttpRequest>& HttpRequest, FString& OutError)
{
    return Natives::FOnlineSubsystemMcp_QueueRequestForClient(this, Service, ClientId, HttpRequest, OutError);
}
