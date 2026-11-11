#pragma once

#include "CoreGlobals.h"
#include "Delegates/Delegate.h"
#include "ObjectLayout.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/PendingNetGame.h"
#include "Templates/Casts.h"
#include "UObject/Object.h"
#include "UObject/UnrealNames.h"

class AActor;
class AGameModeBase;
class AGameStateBase;
class AGameNetworkManager;
class APawn;
class AWorldSettings;
class UDemoNetDriver;
class ULevel;
class ULevelStreaming;
class UGameInstance;
class UNetDriver;
class UNavigationSystem;
class FTimerManager;
struct FTransform;

struct FActorSpawnParameters
{
    FActorSpawnParameters()
        : Name(NAME_None), Template(nullptr), Owner(nullptr), Instigator(nullptr), OverrideLevel(nullptr), SpawnCollisionHandlingOverride(ESpawnActorCollisionHandlingMethod::Undefined), bRemoteOwned(false), bNoFail(false), bDeferConstruction(false), bAllowDuringConstructionScript(false), ObjectFlags(RF_Transactional)
    {
    }

    FName Name;
    AActor* Template;
    AActor* Owner;
    APawn* Instigator;
    ULevel* OverrideLevel;
    ESpawnActorCollisionHandlingMethod SpawnCollisionHandlingOverride;

private:
    uint16_t bRemoteOwned : 1;

public:
    bool IsRemoteOwned() const { return bRemoteOwned; }

    uint16_t bNoFail : 1;
    uint16_t bDeferConstruction : 1;
    uint16_t bAllowDuringConstructionScript : 1;
    EObjectFlags ObjectFlags;
};

static_assert(sizeof(FActorSpawnParameters) == 0x30);

struct FLevelCollection
{
    DefineUnrealStruct(FLevelCollection, Engine)
    DefineUProperty(UNetDriver*, NetDriver)
    DefineUProperty(UDemoNetDriver*, DemoNetDriver)

public:
    UNetDriver* GetNetDriver() const { return NetDriver; }
    void SetNetDriver(UNetDriver* const InNetDriver) { NetDriver = InNetDriver; }
    UDemoNetDriver* GetDemoNetDriver() const { return DemoNetDriver; }
    void SetDemoNetDriver(UDemoNetDriver* const InDemoNetDriver) { DemoNetDriver = InDemoNetDriver; }
};

class UWorldProxy
{
public:
    UWorld* operator->() { return *Natives::GWorld; }
    const UWorld* operator->() const { return *Natives::GWorld; }
    UWorld& operator*() { return **Natives::GWorld; }
    const UWorld& operator*() const { return **Natives::GWorld; }
    operator UWorld*() const { return *Natives::GWorld; }
    UWorld* GetReference() { return *Natives::GWorld; }
};

inline UWorldProxy GWorld;

// DECLARE_MULTICAST_DELEGATE_OneParam(FOnActorSpawned, AActor*): UWorld::SpawnActor's last act, with the new actor.
using FOnActorSpawned = TMulticastDelegate<AActor*>;

class UWorld final : public UObject, public FNetworkNotify
{
    DefineUnrealClass(UWorld, Engine)
    DefineUProperty(ULevel*, PersistentLevel)
    DefineUProperty(UNetDriver*, NetDriver)
    DefineUProperty(UDemoNetDriver*, DemoNetDriver)
    DefineUProperty(AGameModeBase*, AuthorityGameMode)
    DefineUProperty(AGameStateBase*, GameState)
    DefineUProperty(TArray<ULevelStreaming*>, StreamingLevels)
    DefineUProperty(UGameInstance*, OwningGameInstance)
    DefineUProperty(AGameNetworkManager*, NetworkManager)
    DefineUProperty(ULevel*, CurrentLevelPendingVisibility)
    DefineUProperty(ULevel*, CurrentLevelPendingInvisibility)
    DefineUProperty(UNavigationSystem*, NavigationSystem)
    DefineNativeMember(float, NextSwitchCountdown, UWorld_NextSwitchCountdown)
    DefineNativeMember(float, TimeSeconds, UWorld_TimeSeconds)
    DefineNativeMember(FTimerManager*, TimerManager, UWorld_TimerManager)
    // Unreflected: where AddOnActorSpawnedHandler found it (Natives::UWorld_OnActorSpawned).
    DefineNativeObject(FOnActorSpawned, OnActorSpawned, UWorld_OnActorSpawned)

public:
    // Epic's: InHandler runs after each actor the world spawns (OnActorSpawned.Add), until RemoveOnActorSpawnedHandler.
    // The first call finds OnActorSpawned in the build, from the broadcast UWorld::SpawnActor (Natives::UWorld_SpawnActor)
    // ends with; an invalid handle where it isn't found or Core can't bind to native delegates (logged once).
    FDelegateHandle AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate&& InHandler);
    void RemoveOnActorSpawnedHandler(FDelegateHandle InHandle);
    // Finds OnActorSpawned in the build once (game thread; logged). False where it isn't found.
    static bool ResolveOnActorSpawned();

    bool Listen(FURL& InURL);

    ENetMode GetNetMode() const;
    bool IsNetMode(ENetMode Mode) const;
    ENetMode InternalGetNetMode() const;
    ENetMode AttemptDeriveFromURL() const;

    FLevelCollection* FindCollectionByType(const ELevelCollectionType InType);
    UNetDriver* GetNetDriver() const { return NetDriver; }
    void SetNetDriver(UNetDriver* NewDriver) { NetDriver = NewDriver; }
    float GetTimeSeconds() const { return TimeSeconds; }
    AGameModeBase* GetAuthGameMode() const { return AuthorityGameMode; }
    AGameStateBase* GetGameState() const { return GameState; }
    UNavigationSystem* GetNavigationSystem() const { return NavigationSystem; }
    // The owning game instance's timer manager, else the world's own (Epic's inline UWorld::GetTimerManager). Where
    // FTimerManager::ResolveNatives failed, Core's (FTimerManager::Fallback).
    FTimerManager& GetTimerManager() const;
    static ptrdiff_t NavigationSystemOffset() { return Member_NavigationSystem.GetOffsetChecked(); }
    static ptrdiff_t OwningGameInstanceOffset() { return Member_OwningGameInstance.GetOffsetChecked(); }
    AWorldSettings* GetWorldSettings(bool bCheckStreamingPersistent = false, bool bChecked = true) const;
    void AddNetworkActor(AActor* Actor);
    ULevelStreaming* GetLevelStreamingForPackageName(FName InPackageName);

    AActor* SpawnActor(UClass* InClass, FTransform const* UserTransformPtr, const FActorSpawnParameters& SpawnParameters = FActorSpawnParameters());

    template <class T>
    T* SpawnActor(UClass* Class, const FActorSpawnParameters& SpawnParameters = FActorSpawnParameters())
    {
        return Cast<T>(SpawnActor(Class, nullptr, SpawnParameters));
    }

    static void Hook();

private:
    static bool ListenHK(UWorld* World, FURL& InURL);
    static ENetMode InternalGetNetModeHK(const UWorld* World);
};

inline ENetMode UWorld::GetNetMode() const
{
    if (IsRunningDedicatedServer())
    {
        return NM_DedicatedServer;
    }

    return InternalGetNetMode();
}

inline bool UWorld::IsNetMode(ENetMode Mode) const
{
    if (Mode == NM_DedicatedServer)
    {
        return IsRunningDedicatedServer();
    }
    else
    {
        return !IsRunningDedicatedServer() && (InternalGetNetMode() == Mode);
    }
}
