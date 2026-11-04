#pragma once

#include "Building/BuildingSMActor.h"

class AFortPawn;
class UObject;

// A wall piece; one with a door (DoorMesh) opens and closes. Its state replicates: bDoorOpen (the door's swing,
// OnRep_bDoorOpen) and bDoorCollisionDisabled (players walk through it, OnRep_bDoorCollisionDisabled).
class ABuildingWall : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingWall, FortniteGame)

public:
    DefineUProperty(UObject*, DoorMesh)
    // The door's own mesh component: what a player's interaction trace hits (FInteractionType's InteractComponent).
    DefineUProperty(UObject*, DoorComponent)
    DefineBitfieldUProperty(bDoorOpen)
    DefineBitfieldUProperty(bDoorCollisionDisabled)
    static bool HasDoor() { return Member_DoorMesh.IsAvailable() && Member_DoorComponent.IsAvailable() && Member_bDoorOpen.IsAvailable() && Member_bDoorCollisionDisabled.IsAvailable(); }

    bool IsDoor() const;
    // The wall's IFortInteractInterface::ServerOnAttemptInteract, as AFortPlayerController::ServerAttemptInteract calls it
    // for a player's interact on the door: an IT_Simple interaction by RequestingPawn on the door component. The game's
    // own door code then opens it (3.1 0xA87F30: the swing away from the pawn, its collision and nav link, the timers)
    // or closes an open one. False when the wall refused it or the build's interface slot wasn't found.
    bool ServerOnAttemptInteract(AFortPawn* RequestingPawn);
};
