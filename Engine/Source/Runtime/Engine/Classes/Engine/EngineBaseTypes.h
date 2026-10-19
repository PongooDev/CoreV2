#pragma once

#include "Reflection.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"

enum ENetMode
{
    NM_Standalone,
    NM_DedicatedServer,
    NM_ListenServer,
    NM_Client,
    NM_MAX,
};

namespace ENetworkFailure
{
    enum Type
    {
        NetDriverAlreadyExists,
        NetDriverCreateFailure,
        NetDriverListenFailure,
        ConnectionLost,
        ConnectionTimeout,
        FailureReceived,
        OutdatedClient,
        OutdatedServer,
        PendingConnectionFailure,
        NetGuidMismatch,
        NetChecksumMismatch,
    };
}

struct FURL
{
    DefineUnrealStruct(FURL, Engine)
    DefineUProperty(FString, Protocol)
    DefineUProperty(FString, Host)
    DefineUProperty(int32_t, Port)
    DefineUProperty(FString, Map)
    DefineUProperty(FString, RedirectURL)
    DefineUProperty(TArray<FString>, Op)
    DefineUProperty(FString, Portal)
    DefineUProperty(int32_t, Valid)
};
