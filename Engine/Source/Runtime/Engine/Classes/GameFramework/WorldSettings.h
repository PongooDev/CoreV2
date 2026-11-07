#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Math/Vector.h"

class UNavigationSystemConfig;
class UNetConnection;

struct FNetViewer
{
    UNetConnection* Connection = nullptr;
    AActor* InViewer = nullptr;
    AActor* ViewTarget = nullptr;
    FVector ViewLocation;
    FVector ViewDir;

    FNetViewer() = default;
    FNetViewer(UNetConnection* InConnection, float DeltaSeconds);
};

static_assert(sizeof(FNetViewer) == 0x30);

class AWorldSettings : public AActor
{
    DefineUnrealClass(AWorldSettings, Engine)

public:
    DefineUProperty(TArray<FNetViewer>, ReplicationViewers)
    // Whether the world gets a navigation system (UNavigationSystem::CreateNavigationSystem checks it).
    DefineBitfieldUProperty(bEnableNavigationSystem)
    // 4.2 (UE 4.20): what the world's navigation system is made from (FNavigationSystem::AddNavigationSystemToWorld);
    // null when the world has none.
    DefineUProperty(UNavigationSystemConfig*, NavigationSystemConfig)

public:
    static bool HasNavigationSystemConfig() { return Member_NavigationSystemConfig.IsAvailable(); }
    static ptrdiff_t NavigationSystemConfigOffset() { return Member_NavigationSystemConfig.GetOffsetChecked(); }
};
