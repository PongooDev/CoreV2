#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "Math/Vector.h"

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
};
