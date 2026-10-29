#pragma once

#include "Abilities/FortGameplayAbility.h"
#include "Building/BuildingSMActor.h"
#include "FortTeamInfo.h"

#include <cstdint>

class AFortPlayerController;
class UFortAbilitySet;

// A trap: placed by a player (AFortDecoTool) or loaded from the world record.
class ABuildingTrap : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingTrap, FortniteGame)

public:
    // Which pawns set it off. A damage trap keeps ABuildingTrap's default (1.8.2 0x486470): its team's pawns are left out,
    // the rest are hit. A jump or launch pad's blueprint lets its team's pawns in.
    DefineUStructProperty(FFortTargetFilter, TriggerFilter)
    DefineUProperty(UFortAbilitySet*, AbilitySet)

    static void HookVTables();

private:
    void TakePlacerTeam(AFortPlayerController* Placer);
    // A trap set off by its own team (a jump or launch pad) helps whoever steps on it in Battle Royale, enemies included.
    void LetEnemiesUse();

    static void DecoSpawnedHK(AFortPlayerController* Placer, ABuildingSMActor* Spawned, void* Arg);
    static inline void (*DecoSpawnedOG)(AFortPlayerController* Placer, ABuildingSMActor* Spawned, void* Arg) = nullptr;
};
