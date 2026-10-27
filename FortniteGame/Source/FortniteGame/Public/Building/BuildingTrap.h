#pragma once

#include "Building/BuildingSMActor.h"
#include "FortTeamInfo.h"

#include <cstdint>

// A trap: placed by a player (AFortDecoTool) or loaded from the world record.
class ABuildingTrap : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingTrap, FortniteGame)

public:
    // ABuildingActor's. A trap's targets are filtered by it: a pawn on its team is friendly (AFortGameState 0x583360).
    DefineUProperty(EFortTeam, Team)

    static void HookVTables();

private:
    void TakePlacerTeam();

    static void InitializeKismetSpawnedBuildingActorHK(ABuildingTrap* This, uint64_t InitialState, uint64_t InOwnerPersistentID, uint64_t Arg3, void* Arg4, uint64_t Arg5);
    static inline void (*InitializeKismetSpawnedBuildingActorOG)(ABuildingTrap* This, uint64_t InitialState, uint64_t InOwnerPersistentID, uint64_t Arg3, void* Arg4, uint64_t Arg5) = nullptr;
};
