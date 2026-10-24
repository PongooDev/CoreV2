#pragma once

#include "Containers/UnrealString.h"
#include "Interfaces/IHttpRequest.h"
#include "ObjectLayout.h"
#include "StructArray.h"
#include "Templates/SharedPointer.h"
#include "UObject/Object.h"

struct FMcpAddItemRequest
{
    DefineUnrealStruct(FMcpAddItemRequest, McpProfileSys)

public:
    DefineUProperty(FString, ItemId)
    DefineUProperty(FString, TemplateId)
    DefineUProperty(int32_t, Quantity)
};

struct FMcpRemoveItemRequest
{
    DefineUnrealStruct(FMcpRemoveItemRequest, McpProfileSys)
};

struct FMcpChangeQuantityRequest
{
    DefineUnrealStruct(FMcpChangeQuantityRequest, McpProfileSys)
};

struct FMcpChangeAttributesRequest
{
    DefineUnrealStruct(FMcpChangeAttributesRequest, McpProfileSys)
};

struct FMcpProfileChangeRequest
{
    DefineUnrealStruct(FMcpProfileChangeRequest, McpProfileSys)

public:
    DefineUStructArrayProperty(FMcpAddItemRequest, AddRequests)
    DefineUStructArrayProperty(FMcpRemoveItemRequest, RemoveRequests)
    DefineUStructArrayProperty(FMcpChangeQuantityRequest, ChangeQuantityRequests)
    DefineUStructArrayProperty(FMcpChangeAttributesRequest, ChangeAttributesRequests)
    DefineUProperty(FScriptArray, ChangeStatRequests)
};

class UMcpProfile : public UObject
{
    DefineUnrealClass(UMcpProfile, McpProfileSys)

public:
    void PrepareRequest(const TSharedRef<IHttpRequest>& HttpRequest, uint32_t ResponseType)
    {
        CallVirtual<void, UMcpProfile, const TSharedRef<IHttpRequest>&, uint32_t>(this, Natives::UMcpProfile_PrepareRequestIndex, HttpRequest, ResponseType);
    }
};
