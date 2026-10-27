#pragma once

#include "Building/BuildingSMActor.h"
#include "FortTeamInfo.h"

#include <cstdint>

class AFortPlayerController;

// A trap: placed by a player (AFortDecoTool) or loaded from the world record.
class ABuildingTrap : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingTrap, FortniteGame)

public:
    // ABuildingActor's. A trap's targets are filtered by it: a pawn on its team is friendly (AFortGameState 0x583360).
    DefineUProperty(EFortTeam, Team)

    static void HookVTables();

private:
    void TakePlacerTeam(AFortPlayerController* Placer);

    static void DecoSpawnedHK(AFortPlayerController* Placer, ABuildingSMActor* Spawned, void* Arg);
    static inline void (*DecoSpawnedOG)(AFortPlayerController* Placer, ABuildingSMActor* Spawned, void* Arg) = nullptr;
};
