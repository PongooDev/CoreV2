#pragma once

#include "Building/BuildingActor.h"
#include "Reflection.h"

// The rising floor of the Fill mutator (from 8.40; LavaFloor_C, Food Fight: Deep Fried's LavaFloor_Child_C), an
// ABuildingFillFloor in 21.40 (Core declares no ABuildingGameplayActor, so this derives from its ABuildingActor). Its
// natives are all in 8.40 (CL-6005771): the Fill mutator's OnGamePhaseChanged starts it (0xEAB080), each step moves it
// to its FloorPositionData's Height over MoveTime (UpdateToNextStep 0xEB2B80, MoveTo 0xEAB2A0), and its tick (0xEACB50)
// stops it once it is over CapHeight. Nothing in the build sets CapHeight but the Fill mutator's OnPlaylistLoaded
// (0xFE9F00) from its LavaCapHeight as it spawns the floor: the Barrier mutator's stripped server code set it.
class AAthenaFillFloor : public ABuildingActor
{
    DefineUnrealClass(AAthenaFillFloor, FortniteGame)

public:
    DefineUProperty(float, FloorZ)

    // The build's CapHeight setter (8.40 0xEA91B0; Core's name, 21.40's PDB names only the member, ABuildingFillFloor::
    // CapHeight): the height the floor stops at (the unreflected CapHeight, right after FloorZ). With bUpdateMovement a moving floor is sent to it, a waiting one's next step ends there.
    // False where the build's function isn't found (Finder::ResolveFillFloorSetCapHeightLive).
    bool SetCapHeight(float NewCapHeight, bool bUpdateMovement);
    // Whether the build's SetCapHeight is found; resolved once (the server's boot asks, so its finder line is logged).
    static bool HasSetCapHeight();
};
