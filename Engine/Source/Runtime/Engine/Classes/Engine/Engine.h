#pragma once

#include "ObjectLayout.h"
#include "Engine/EngineBaseTypes.h"
#include "UObject/Object.h"

class UNetDriver;
class UWorld;

class FOnNetworkFailure
{
public:
    void Broadcast(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString) const;
};

struct FWorldContext
{
    DefineNativeMember(bool, RunAsDedicated, FWorldContext_RunAsDedicated)
};

class UEngine : public UObject
{
    DefineUnrealClass(UEngine, Engine)
    DefineUProperty(UClass*, ConsoleClass)
    DefineUProperty(float, NetClientTicksPerSecond)
    DefineNativeMember(FOnNetworkFailure, NetworkFailureEvent, UEngine_NetworkFailureEvent)

public:
    FOnNetworkFailure& OnNetworkFailure() { return NetworkFailureEvent; }

    void BroadcastNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString = FString())
    {
        NetworkFailureEvent.Broadcast(World, NetDriver, FailureType, ErrorString);
    }

    bool CreateNamedNetDriver(UWorld* InWorld, FName NetDriverName, FName NetDriverDefinition);
    UNetDriver* FindNamedNetDriver(const UWorld* InWorld, FName NetDriverName);

    float GetMaxTickRate(float DeltaTime, bool bAllowFrameRateSmoothing = true) const;

    static void Hook();
};

class FEngineGlobal
{
public:
    UEngine* operator->() const { return *Natives::GEngine; }
    operator UEngine*() const { return *Natives::GEngine; }
};

inline constexpr FEngineGlobal GEngine;
