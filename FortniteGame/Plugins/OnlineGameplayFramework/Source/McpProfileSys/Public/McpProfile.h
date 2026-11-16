#pragma once

#include "Containers/UnrealString.h"
#include "Delegates/Delegate.h"
#include "Dom/JsonValue.h"
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

    // The delegate HandleProfileUpdate (7.00 0x955BE0) broadcasts once an update (a query's whole profile or a command's
    // changes) has changed the profile's items: 21.40's ItemsUpdatedDelegate. 7.00's takes one 8-byte argument (the
    // update's, `mov rdx, [rsi]`), which Core's handlers don't read. Found from 7.00 on (Creative), else unavailable.
    using FOnItemsUpdated = TMulticastDelegate<int64_t>;
    DefineNativeObject(FOnItemsUpdated, ItemsUpdatedDelegate, UMcpProfile_ItemsUpdatedDelegate)
    static bool HasItemsUpdatedDelegate() { return Natives::UMcpProfile_ItemsUpdatedDelegate > 0 && Natives::IDelegateInstance_GetHandleIndexResolved; }

    // One of the profile's stats (its "stats.attributes" entry by that name), as the profile's state holds it: null when
    // the profile has no such stat, or Core hasn't the getter (Natives::UMcpProfile_GetStat, from 7.00).
    TSharedPtr<FJsonValue, ESPMode::Fast> GetStat(const FString& StatName) const
    {
        TSharedPtr<FJsonValue, ESPMode::Fast> Result;
        if (Natives::UMcpProfile_GetStat)
        {
            Natives::UMcpProfile_GetStat(this, &Result, StatName);
        }
        return Result;
    }

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
        Natives::UMcpProfile_QueryProfile(this, OnComplete, false);
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
