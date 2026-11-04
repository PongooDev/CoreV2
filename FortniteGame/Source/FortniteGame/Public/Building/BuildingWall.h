#pragma once

#include "Building/BuildingSMActor.h"

class UObject;

// A wall piece; one with a door (DoorMesh) opens and closes. Its state replicates: bDoorOpen (the door's swing,
// OnRep_bDoorOpen) and bDoorCollisionDisabled (players walk through it, OnRep_bDoorCollisionDisabled).
class ABuildingWall : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingWall, FortniteGame)
    DefineUFunction(OnRep_bDoorOpen)
    DefineUFunction(OnRep_bDoorCollisionDisabled)

public:
    DefineUProperty(UObject*, DoorMesh)
    DefineBitfieldUProperty(bDoorOpen)
    DefineBitfieldUProperty(bDoorCollisionDisabled)
    static bool HasDoor() { return Member_DoorMesh.IsAvailable() && Member_bDoorOpen.IsAvailable() && Member_bDoorCollisionDisabled.IsAvailable(); }

    bool IsDoor() const;
    // A pawn without a player controller opens the door as a player's interaction does on the server: the door's
    // open state and its collision, with their OnReps run here (the server's own copy) and replicated to the clients.
    void OpenDoor();
};
