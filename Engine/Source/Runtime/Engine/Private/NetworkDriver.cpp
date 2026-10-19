#include "pch.h"
#include "Engine/NetDriver.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"

bool UNetDriver::InitListen(FNetworkNotify* InNotify, FURL& ListenURL, bool bReuseAddressAndPort, FString& Error)
{
    if (Natives::UNetDriver_InitListenIndex < 0 && !Finder::ResolveInitListen())
    {
        return false;
    }

    using FInitListen = bool (*)(UNetDriver*, FNetworkNotify*, FURL&, bool, FString&);
    void** VTable = *reinterpret_cast<void***>(this);
    return reinterpret_cast<FInitListen>(VTable[Natives::UNetDriver_InitListenIndex])(this, InNotify, ListenURL, bReuseAddressAndPort, Error);
}

ENetMode UNetDriver::GetNetMode() const
{
    return static_cast<ENetMode>(Natives::UNetDriver_GetNetMode(this));
}

void UNetDriver::SetWorld(UWorld* InWorld)
{
    Natives::UNetDriver_SetWorld(this, InWorld);
}
