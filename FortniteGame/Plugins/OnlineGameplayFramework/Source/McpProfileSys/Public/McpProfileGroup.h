#pragma once

#include "GameFramework/OnlineReplStructs.h"
#include "Interfaces/IHttpRequest.h"
#include "ObjectLayout.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"

class FOnlineSubsystemMcp;
class UMcpProfile;

enum EContextCredentials : int32_t
{
    CXC_Client,
    CXC_DedicatedServer,
    CXC_Cheater,
    CXC_Public,
};

struct FProfileHttpRequest
{
    DefineNativeMember(UMcpProfile*, Profile, FProfileHttpRequest_Profile)
    DefineNativeMember(FHttpRequestPtr, HttpRequest, FProfileHttpRequest_HttpRequest)
    DefineNativeMember(EContextCredentials, ContextCredentials, FProfileHttpRequest_ContextCredentials)
    DefineNativeMember(uint32_t, ResponseType, FProfileHttpRequest_ResponseType)
};

class UMcpProfileGroup : public UObject
{
    DefineUnrealClass(UMcpProfileGroup, McpProfileSys)

public:
    DefineNativeMember(FOnlineSubsystemMcp*, McpSubsystem, UMcpProfileGroup_McpSubsystem)
    DefineNativeObject(FUniqueNetIdRepl, CredentialsAccountId, UMcpProfileGroup_CredentialsAccountId)

    void SendRequestNow(FProfileHttpRequest& Request);
    // 7.40 onwards: the request was prepared by the caller, and its context credentials come with it.
    void SendRequestNow(const TSharedRef<IHttpRequest>& HttpRequest, EContextCredentials ContextCredentials);

    static void Hook();

private:
    static void SendRequestNowHK(UMcpProfileGroup* This, FProfileHttpRequest& Request);
    static void SendHttpRequestNowHK(UMcpProfileGroup* This, const TSharedRef<IHttpRequest>& HttpRequest, EContextCredentials ContextCredentials);
};
