#include "pch.h"
#include "McpProfileGroup.h"
#include "Finder.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "McpProfile.h"
#include "McpServiceConfig.h"
#include "Natives.h"
#include "OnlineSubsystemMcp.h"
#include "Online/NotForLicensees/FortMcpConfig.h"

DEFINE_LOG_CATEGORY_STATIC(LogProfileSys, Log, All);

void UMcpProfileGroup::SendRequestNow(FProfileHttpRequest& Request)
{
    if (Request.Profile)
    {
        Request.Profile->PrepareRequest(Request.HttpRequest.ToSharedRef(), Request.ResponseType);
    }

    SendRequestNow(Request.HttpRequest.ToSharedRef(), Request.ContextCredentials);
}

void UMcpProfileGroup::SendRequestNow(const TSharedRef<IHttpRequest>& HttpRequest, EContextCredentials ContextCredentials)
{
    UE_LOG(LogProfileSys, Log, TEXT("MCP-Profile: Dispatching request to %s"), *HttpRequest->GetURL());

    bool bStarted = false;
    FString ErrorStr;
    FGameServiceMcpPtr McpService = McpSubsystem->GetMcpGameService();
    const FUniqueNetIdRepl& AccountId = CredentialsAccountId;

    switch (ContextCredentials)
    {
    case CXC_Client:
    case CXC_Public:
        if (AccountId.IsValid())
        {
            McpSubsystem->QueueRequest(*McpService, *AccountId, HttpRequest, ErrorStr);
            bStarted = true;
        }
        else
        {
            ErrorStr = TEXT("Invalid client CredentialsAccountId.");
        }
        break;
    case CXC_DedicatedServer:
    {
#if WITH_SERVER_CODE
        const FMcpServicePermissions* Permissions = McpService->ServiceConfig.FindServicePermissions(DedicatedServerServicePermissionsName);
        if (!Permissions)
        {
            // On a packed exe the service config is read during PreInit, before Core's FortMcpConfig hook is enabled
            // (it waits for the packer's code check), so the server's permissions were never added. Core adds them to
            // every service once its hooks are in; should that have missed them, add them now, to all of them.
            if (Finder::ResolveMcpBasicAuth())
            {
                FFortMcpConfig::AddDedicatedServerPermissionsToServices(McpSubsystem);
            }
            FFortMcpConfig::AddDedicatedServerPermissions(McpService->ServiceConfig);
            Permissions = McpService->ServiceConfig.FindServicePermissions(DedicatedServerServicePermissionsName);
        }
        if (Permissions)
        {
            bStarted = McpSubsystem->QueueRequest(*McpService, Permissions->Id, HttpRequest, ErrorStr);
        }
        else
        {
            ErrorStr = TEXT("Unable to find service permissions for server.");
        }
#else
        ErrorStr = TEXT("FATAL: Should not be possible to issue dedicated server commands without WITH_SERVER_CODE");
#endif
        break;
    }
    case CXC_Cheater:
        ErrorStr = TEXT("Sending cheat commands from the client/dedicated server is no longer supported");
        break;
    default:
        ErrorStr = TEXT("Invalid request context.");
        break;
    }

    if (!ErrorStr.IsEmpty())
    {
        UE_LOG(LogProfileSys, Warning, TEXT("SendRequestNow Error: %s"), *ErrorStr);
    }

    if (!bStarted)
    {
        UE_LOG(LogProfileSys, Warning, TEXT("Failed to start profile request."));
        HttpRequest->CancelRequest();
    }
}

void UMcpProfileGroup::Hook()
{
    if (Natives::UMcpProfileGroup_SendRequestNowTakesHttpRequest)
    {
        Hooks::Detour(L"UMcpProfileGroup::SendRequestNow", reinterpret_cast<void*>(Natives::UMcpProfileGroup_SendRequestNow), &SendHttpRequestNowHK, nullptr);
        return;
    }
    Hooks::Detour(L"UMcpProfileGroup::SendRequestNow", reinterpret_cast<void*>(Natives::UMcpProfileGroup_SendRequestNow), &SendRequestNowHK, nullptr);
}

void UMcpProfileGroup::SendRequestNowHK(UMcpProfileGroup* This, FProfileHttpRequest& Request)
{
    This->SendRequestNow(Request);
}

void UMcpProfileGroup::SendHttpRequestNowHK(UMcpProfileGroup* This, const TSharedRef<IHttpRequest>& HttpRequest, EContextCredentials ContextCredentials)
{
    This->SendRequestNow(HttpRequest, ContextCredentials);
}
