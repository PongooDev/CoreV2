#pragma once

#include "CoreGlobals.h"
#include "ObjectLayout.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/EngineTypes.h"
#include "Engine/PendingNetGame.h"
#include "UObject/Object.h"

class AActor;
class AGameModeBase;
class AGameNetworkManager;
class AWorldSettings;
class UDemoNetDriver;
class ULevel;
class UNetDriver;

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

class UWorld final : public UObject, public FNetworkNotify
{
    DefineUnrealClass(UWorld, Engine)
    DefineUProperty(UNetDriver*, NetDriver)
    DefineUProperty(UDemoNetDriver*, DemoNetDriver)
    DefineUProperty(AGameModeBase*, AuthorityGameMode)
    DefineUProperty(AGameNetworkManager*, NetworkManager)
    DefineUProperty(ULevel*, CurrentLevelPendingVisibility)
    DefineUProperty(ULevel*, CurrentLevelPendingInvisibility)
    DefineNativeMember(float, NextSwitchCountdown, UWorld_NextSwitchCountdown)
    DefineNativeMember(float, TimeSeconds, UWorld_TimeSeconds)

public:
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
    AWorldSettings* GetWorldSettings(bool bCheckStreamingPersistent = false, bool bChecked = true) const;
    void AddNetworkActor(AActor* Actor);

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
