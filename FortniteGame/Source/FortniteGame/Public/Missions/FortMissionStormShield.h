#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "UObject/Stack.h"

class AFortMissionStormSafeZone;
struct FVector;

// Save the World's storm shield (StW_Storm_SafeArea_*): the shield's radius and, from 6.10, the safe areas it spawns
// from SafeAreaTemplate, which replicate in SafeAreas for the clients' maps.
class AFortMissionStormShield : public AActor
{
    DefineUnrealClass(AFortMissionStormShield, FortniteGame)
    DefineUFunction(SpawnSafeAreaAtLocation)
    DefineUFunction(RemoveAndDestroySafeArea)

public:
    DefineUProperty(UClass*, SafeAreaTemplate)
    DefineUProperty(TArray<AFortMissionStormSafeZone*>, SafeAreas)

    AFortMissionStormSafeZone* SpawnSafeAreaAtLocation(const FVector& InLocation, const float InRadius);
    bool RemoveAndDestroySafeArea(AFortMissionStormSafeZone* AreaToRemove);
    DECLARE_FUNCTION(execSpawnSafeAreaAtLocation);
    DECLARE_FUNCTION(execRemoveAndDestroySafeArea);

    static void HookNatives();
};
