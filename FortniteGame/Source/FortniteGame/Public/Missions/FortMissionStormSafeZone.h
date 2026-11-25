#pragma once

#include "GameFramework/Actor.h"
#include "UObject/Stack.h"

// A bubble safe from the storm that a storm shield spawns (AFortMissionStormShield::SpawnSafeAreaAtLocation; 9.00's
// Wargames "Safety Zone": ShieldGeneratorActor_C's generators). SafeZoneRadius replicates (OnRep_ZoneRadius scales
// the mesh and calls the Blueprint's OnSafeZoneRadiusChanged).
class AFortMissionStormSafeZone : public AActor
{
    DefineUnrealClass(AFortMissionStormSafeZone, FortniteGame)
    DefineUFunction(SetSafeZoneRadius)
    DefineUFunction(OnRep_ZoneRadius)

public:
    DefineUProperty(float, SafeZoneRadius)
    static bool HasSafeZoneRadius() { return Member_SafeZoneRadius.IsAvailable(); }

    void SetSafeZoneRadius(const float InRadius);
    void OnRep_ZoneRadius();
    DECLARE_FUNCTION(execSetSafeZoneRadius);

    static void HookNatives();
};
