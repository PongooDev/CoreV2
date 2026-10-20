#pragma once

#include "GameFramework/Actor.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"

class AFortPawn;
class AFortPlayerController;
class UWorld;
struct FFortItemEntry;

class AFortPickup : public AActor
{
    DefineUnrealClass(AFortPickup, FortniteGame)
    DefineUFunction(TossPickup)

public:
    static AFortPickup* CreatePickup(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation = nullptr, const FRotator* SpawnRotation = nullptr, AFortPlayerController* OptionalOwner = nullptr, UClass* OverrideClass = nullptr);
    void TossPickup(const FVector& FinalLocation, AFortPawn* ItemOwner, int32_t OverrideMaxStackCount);
};
