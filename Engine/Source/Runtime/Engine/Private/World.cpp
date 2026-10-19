#include "pch.h"
#include "Engine/World.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Natives.h"

DEFINE_LOG_CATEGORY_STATIC(LogWorld, Log, All);

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
        UE_LOG(LogWorld, Log, TEXT("Failed to listen: %s"), *Error);
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

AWorldSettings* UWorld::GetWorldSettings(bool bCheckStreamingPersistent, bool bChecked) const
{
    return Natives::UWorld_GetWorldSettings(this, bCheckStreamingPersistent, bChecked);
}

void UWorld::AddNetworkActor(AActor* Actor)
{
    Natives::UWorld_AddNetworkActor(this, Actor);
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

bool UWorld::ListenHK(UWorld* World, FURL& InURL)
{
    return World->Listen(InURL);
}

ENetMode UWorld::InternalGetNetModeHK(const UWorld* World)
{
    return World->InternalGetNetMode();
}

void UWorld::Hook()
{
    Hooks::Detour(L"UWorld::InternalGetNetMode", reinterpret_cast<void*>(Natives::UWorld_InternalGetNetMode), reinterpret_cast<void*>(&InternalGetNetModeHK), nullptr);

    for (const Sites::FInlinedNetMode& NetMode : Sites::InlinedNetMode)
    {
        if (!NetMode.Branch || !NetMode.DemoNetDriverDisplacement)
        {
            continue;
        }

        const uint8_t Branch[] = { NetMode.Branch[0] == 0x74 ? static_cast<uint8_t>(0xEB) : static_cast<uint8_t>(0x90), NetMode.Branch[0] == 0x74 ? NetMode.Branch[1] : static_cast<uint8_t>(0x90) };
        const uint8_t Displacement[] = { NetMode.NetDriverDisplacement };
        Hooks::WriteBytes(L"UWorld::InternalGetNetMode inlined branch", NetMode.Branch, Branch);
        Hooks::WriteBytes(L"UWorld::InternalGetNetMode inlined driver", NetMode.DemoNetDriverDisplacement, Displacement);
    }

    Hooks::RedirectCall(L"UWorld::Listen", Sites::LoadMapListenCall, reinterpret_cast<void*>(&ListenHK));
}
