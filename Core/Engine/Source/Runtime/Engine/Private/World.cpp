#include "pch.h"
#include "Engine/World.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Log.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Natives.h"

bool UWorld::Listen(FURL& InURL)
{
#if WITH_SERVER_CODE
    if (NetDriver)
    {
        GEngine->BroadcastNetworkFailure(this, NetDriver, ENetworkFailure::NetDriverAlreadyExists);
        return false;
    }

    if (GEngine->CreateNamedNetDriver(this, NAME_GameNetDriver, NAME_GameNetDriver))
    {
        NetDriver = GEngine->FindNamedNetDriver(this, NAME_GameNetDriver);
        NetDriver->SetWorld(this);
        FLevelCollection* const SourceCollection = FindCollectionByType(ELevelCollectionType::DynamicSourceLevels);
        if (SourceCollection)
        {
            SourceCollection->SetNetDriver(NetDriver);
        }
        FLevelCollection* const StaticCollection = FindCollectionByType(ELevelCollectionType::StaticLevels);
        if (StaticCollection)
        {
            StaticCollection->SetNetDriver(NetDriver);
        }
    }

    if (NetDriver == nullptr)
    {
        GEngine->BroadcastNetworkFailure(this, nullptr, ENetworkFailure::NetDriverCreateFailure);
        return false;
    }

    FString Error;
    if (!NetDriver->InitListen(this, InURL, false, Error))
    {
        GEngine->BroadcastNetworkFailure(this, NetDriver, ENetworkFailure::NetDriverListenFailure, Error);
        Log::Info(L"Failed to listen: {}", Error.ToStdString());
        NetDriver->SetWorld(nullptr);
        NetDriver = nullptr;
        FLevelCollection* SourceCollection = FindCollectionByType(ELevelCollectionType::DynamicSourceLevels);
        if (SourceCollection)
        {
            SourceCollection->SetNetDriver(nullptr);
        }
        FLevelCollection* StaticCollection = FindCollectionByType(ELevelCollectionType::StaticLevels);
        if (StaticCollection)
        {
            StaticCollection->SetNetDriver(nullptr);
        }
        return false;
    }
    static bool LanPlay = FParse::Param(FCommandLine::Get(), L"lanplay");
    if (!LanPlay && (NetDriver->MaxInternetClientRate < NetDriver->MaxClientRate) && (NetDriver->MaxInternetClientRate > 2500))
    {
        NetDriver->MaxClientRate = NetDriver->MaxInternetClientRate;
    }

    NextSwitchCountdown = NetDriver->ServerTravelPause;
    return true;
#else
    return false;
#endif
}

ENetMode UWorld::InternalGetNetMode() const
{
    if (NetDriver != nullptr)
    {
        const bool bIsClientOnly = IsRunningClientOnly();
        return bIsClientOnly ? NM_Client : NetDriver->GetNetMode();
    }

    if (DemoNetDriver)
    {
        return DemoNetDriver->GetNetMode();
    }

    return AttemptDeriveFromURL();
}

ENetMode UWorld::AttemptDeriveFromURL() const
{
    return static_cast<ENetMode>(Natives::UWorld_AttemptDeriveFromURL(this));
}

FLevelCollection* UWorld::FindCollectionByType(const ELevelCollectionType InType)
{
    return Natives::UWorld_FindCollectionByType(this, InType);
}
