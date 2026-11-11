#pragma once

#include "Athena/FortAthenaVehicle.h"

class UFortAthenaExitCraftInfo;

// The Getaway's van (from 5.40; Bling_Van_C). Its spawner brings it in (AFortAthenaExitCraftSpawner::SpawnExitCraft);
// then its own state machine runs on the server (5.40 0xBCC8C0, its CurrentState: Spawned, Landed, SpawnBalloon,
// GettingIntoPosition, WaitingForPawns, Exiting), and The Getaway's mutator (AFortAthenaMutator_Heist's tick, 0xC4C810)
// takes in a team whose jewel carrier reaches it while it waits. Its departure (0xBB2610) takes its special actor off
// the list (CraftSpecialActorID) as it leaves.
class AFortAthenaExitCraft : public AFortAthenaVehicle
{
    DefineUnrealClass(AFortAthenaExitCraft, FortniteGame)

public:
    // Replicated; the states read their values from it (MinLandingSpeed, ExitZOffset, ExitTime).
    DefineUProperty(UFortAthenaExitCraftInfo*, ExitCraftInfo)
    // The id the van's special actor has on the game state's list: "<SpecialActorCraftTag>_<its name>".
    DefineUProperty(FName, CraftSpecialActorID)
    static ptrdiff_t CraftSpecialActorIDOffset() { return StaticClass() && Member_CraftSpecialActorID.IsAvailable() ? Member_CraftSpecialActorID.GetOffsetChecked() : -1; }

    // Puts the van on every map and compass (the craft info's SpecialActorCraftTag and Craft*Icon*), as its spawner's
    // RegisterSpawnerSpecialActor (5.40 0xBC4A10) puts the spawner there, and keeps its id in CraftSpecialActorID for
    // the departure to take it off. Server; stripped from the client builds with the spawn it belonged to (nothing in
    // 5.40 or 5.41 sets CraftSpecialActorID), so Core supplies it, Epic's way.
    void RegisterCraftSpecialActor();
};
