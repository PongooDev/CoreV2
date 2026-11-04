#pragma once

#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "Reflection.h"

// A spectator camera placed over a part of the Battle Royale map (an ACineCameraActor). The spectating player
// controller cycles through them (AFortPlayerControllerSpectating::ArenaCams). On 3.0 they are in
// /Game/Athena/Maps/Athena_CameraLocations, a sublevel the persistent map streams only on demand, named after the place
// they show (arenaCam_TiltedTowers1 ...); CameraZone, the tag of that place, is left empty there.
class AFortArenaCamera : public AActor
{
    DefineUnrealClass(AFortArenaCamera, FortniteGame)

public:
    DefineUProperty(FGameplayTag, CameraZone)
    bool HasCameraZone() const { return Member_CameraZone.IsAvailable(); }
};
