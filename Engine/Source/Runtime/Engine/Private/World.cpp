#include "pch.h"
#include "Engine/World.h"
#include "Engine/DemoNetDriver.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/LevelStreaming.h"
#include "Engine/NetDriver.h"
#include "Engine/Player.h"
#include "Engine/ReplicationDriver.h"
#include "Finder.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Natives.h"
#include "ReplicationGraph.h"
#include "Templates/Casts.h"
#include "TimerManager.h"

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
    // Core (CoreV2 #377): a dedicated server sends each client at least UNetDriver::CoreMinClientRate; the client build's
    // ini, which the server runs on, has 20000 (5.10), the clients ask for 30000, and RaiseClientNetSpeeds lifts their
    // connections to it.
    const int32_t IniClientRate = NetDriver->MaxClientRate;
    if (NetDriver->MaxClientRate < UNetDriver::CoreMinClientRate)
    {
        NetDriver->MaxClientRate = UNetDriver::CoreMinClientRate;
    }
    if (NetDriver->MaxInternetClientRate < UNetDriver::CoreMinClientRate)
    {
        NetDriver->MaxInternetClientRate = UNetDriver::CoreMinClientRate;
    }
    const UPlayer* PlayerDefault = UPlayer::StaticClass() ? static_cast<const UPlayer*>(UPlayer::StaticClass()->GetDefaultObject()) : nullptr;
    UE_LOG(LogWorld, Log, TEXT("Listen: clients are sent up to %d bytes a second (the game's MaxClientRate %d), clients ask for %d"), NetDriver->MaxClientRate,
        IniClientRate, PlayerDefault ? static_cast<int32_t>(PlayerDefault->ConfiguredInternetSpeed) : -1);

    // Core (CoreV2 #412): the replication graph's connections forget whatever they still hold at the address of each
    // actor this world spawns (UReplicationGraph::HandleActorSpawned), on the builds whose graph leaves a destroyed
    // actor's info with them and whose code for it the finder knows (6.00 to 6.02). The handler goes with the world; a
    // net driver without a replication graph, and a graph of another build, get none.
    if (UReplicationGraph* Graph = NetDriver->HasReplicationDriver() ? Cast<UReplicationGraph>(reinterpret_cast<UObject*>(static_cast<UReplicationDriver*>(NetDriver->ReplicationDriver))) : nullptr;
        Graph && UReplicationGraph::HasConnections() && Finder::ResolveReplicationGraphActorInfoLive(*reinterpret_cast<void***>(Graph)))
    {
        const FDelegateHandle Handle = AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* Actor) { UReplicationGraph::HandleActorSpawned(Actor); }));
        UE_LOG(LogWorld, Log, TEXT("Listen: the replication graph's connections %s (UWorld::OnActorSpawned)"),
            Handle.IsValid() ? TEXT("forget a destroyed actor's info when a new actor is spawned at its address") : TEXT("keep a destroyed actor's info: the world's OnActorSpawned wasn't found"));
    }

    NextSwitchCountdown = NetDriver->ServerTravelPause;
    return true;
#else
    return false;
#endif
}

AActor* UWorld::SpawnActor(UClass* InClass, FTransform const* UserTransformPtr, const FActorSpawnParameters& SpawnParameters)
{
    return Natives::UWorld_SpawnActor(this, InClass, UserTransformPtr, SpawnParameters);
}

AWorldSettings* UWorld::GetWorldSettings(bool bCheckStreamingPersistent, bool bChecked) const
{
    return Natives::UWorld_GetWorldSettings(this, bCheckStreamingPersistent, bChecked);
}

FTimerManager& UWorld::GetTimerManager() const
{
    FTimerManager* Manager = nullptr;
    if (FTimerManager::ResolveNatives())
    {
        UGameInstance* GameInstance = OwningGameInstance;
        Manager = GameInstance ? GameInstance->TimerManager : TimerManager;
    }
    return Manager ? *Manager : FTimerManager::Fallback();
}

void UWorld::AddNetworkActor(AActor* Actor)
{
    Natives::UWorld_AddNetworkActor(this, Actor);
}

FDelegateHandle UWorld::AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate&& InHandler)
{
    return ResolveOnActorSpawned() ? OnActorSpawned.Add(std::move(InHandler)) : FDelegateHandle();
}

bool UWorld::ResolveOnActorSpawned()
{
    // UWorld::SpawnActor (5.30 0x2583120, 5.40 0x25BBF60, 4.4 0x22005B0, 3.2 0x2136730, 1.8.1 0x225B5F0) ends with
    // OnActorSpawned.Broadcast(Actor): inlined from UE 4.21 (5.0-5.40 +0x3E0), a call before (4.4 and 3.2 +0x3E0, 1.8.1
    // +0x390). Its offset is the world's, inside UWorld.
    static const bool bFound = []
    {
        // 1.9.1's moved code starts with a jump to the body.
        const uintptr_t SpawnActor = Natives::UWorld_SpawnActor ? Finder::Unthunk(reinterpret_cast<uintptr_t>(Natives::UWorld_SpawnActor)) : 0;
        const ptrdiff_t Offset = SpawnActor ? Finder::LastBroadcastDelegateOffset(SpawnActor) : -1;
        const int32_t WorldSize = StaticClass() ? StaticClass()->GetPropertiesSize() : 0;
        // The world's default object has it as Epic's constructor left it.
        UObject* WorldDefault = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
        const bool bLooksLikeDelegate = WorldDefault && Offset > 0x28 && Offset < WorldSize
            && FMulticastDelegateBase::IsDefaultConstructedAt(reinterpret_cast<const uint8_t*>(WorldDefault) + Offset);
        if (!bLooksLikeDelegate)
        {
            UE_LOG(LogWorld, Error, TEXT("Finder: UWorld::OnActorSpawned not found (SpawnActor 0x%llX, broadcast +0x%llX, UWorld 0x%X bytes)"),
                static_cast<unsigned long long>(SpawnActor ? SpawnActor - Finder::ImageBase() : 0), static_cast<long long>(Offset), WorldSize);
            return false;
        }
        Natives::UWorld_OnActorSpawned = Offset;
        UE_LOG(LogWorld, Log, TEXT("Finder: UWorld::OnActorSpawned +0x%llX (UWorld::SpawnActor 0x%llX)"), static_cast<long long>(Offset),
            static_cast<unsigned long long>(SpawnActor - Finder::ImageBase()));
        return true;
    }();
    return bFound;
}

void UWorld::RemoveOnActorSpawnedHandler(FDelegateHandle InHandle)
{
    if (Natives::UWorld_OnActorSpawned > 0)
    {
        OnActorSpawned.Remove(InHandle);
    }
}

ULevelStreaming* UWorld::GetLevelStreamingForPackageName(FName InPackageName)
{
    for (ULevelStreaming* LevelStreaming : StreamingLevels)
    {
        if (LevelStreaming && LevelStreaming->GetWorldAssetPackageFName() == InPackageName)
        {
            return LevelStreaming;
        }
    }
    return nullptr;
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
    // 5.0's InternalGetNetMode derives the URL's mode itself: its original, reached only with both net drivers null
    // (InternalGetNetMode above), stands in for AttemptDeriveFromURL.
    void** const DeriveFromURL = Natives::bInternalGetNetModeDerivesFromURL ? reinterpret_cast<void**>(&Natives::UWorld_AttemptDeriveFromURL) : nullptr;
    Hooks::Detour(L"UWorld::InternalGetNetMode", reinterpret_cast<void*>(Natives::UWorld_InternalGetNetMode), reinterpret_cast<void*>(&InternalGetNetModeHK), DeriveFromURL);

    for (const Sites::FInlinedNetMode& NetMode : Sites::InlinedNetMode)
    {
        if (!NetMode.Branch || !NetMode.DemoNetDriverDisplacement)
        {
            continue;
        }

        const uint8_t Branch[] = { NetMode.Branch[0] == 0x74 ? static_cast<uint8_t>(0xEB) : static_cast<uint8_t>(0x90), NetMode.Branch[0] == 0x74 ? NetMode.Branch[1] : static_cast<uint8_t>(0x90) };
        // The whole disp32: DemoNetDriver's is +0x108 on 2.2, so its second byte isn't 0 there.
        const uint8_t Displacement[] = { NetMode.NetDriverDisplacement, 0x00, 0x00, 0x00 };
        Hooks::WriteBytes(L"UWorld::InternalGetNetMode inlined branch", NetMode.Branch, Branch);
        Hooks::WriteBytes(L"UWorld::InternalGetNetMode inlined driver", NetMode.DemoNetDriverDisplacement, Displacement);
    }

    Hooks::RedirectCall(L"UWorld::Listen", Sites::LoadMapListenCall, reinterpret_cast<void*>(&ListenHK));
}
