#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"

#include <cstdint>

class AFortAthenaExitCraft;
class AFortAthenaExitCraftSpawner;
class AFortGameModeAthena;
class AFortGameStateAthena;
class AFortPlayerPawnAthena;

// A playlist modifier's actor (AFortGameplayMutator; from 3.5): AFortGameModeAthena spawns the playlist's ModifierList
// mutators when its data has loaded and keeps them in GameplayMutators.
class AFortGameplayMutator : public AActor
{
    DefineUnrealClass(AFortGameplayMutator, FortniteGame)
};

// A Battle Royale mutator, with the game mode and the game state it caches in BeginPlay.
class AFortAthenaMutator : public AFortGameplayMutator
{
    DefineUnrealClass(AFortAthenaMutator, FortniteGame)

public:
    DefineUProperty(AFortGameModeAthena*, CachedGameMode)
    DefineUProperty(AFortGameStateAthena*, CachedGameState)
};

// Where a getaway van is (EHeistExitCraftState): on its way (its spawner's countdown), landed, or gone with its team.
class EHeistExitCraftState
{
    DefineUnrealEnum(EHeistExitCraftState, FortniteGame)
    DefineUEnumValue(None)
    DefineUEnumValue(Incoming)
    DefineUEnumValue(Spawned)
    DefineUEnumValue(Exited)
};

// One getaway van of the match (FHeistExitCraftData): OnGamePhaseChanged lists one None entry per HeistExitCraftSpawnData
// at the safe zones' start (5.40 0xC56630); the spawn (0xC67CD0) makes the next one Incoming with its spawner and its
// SpawnTime; the Tick (0xC4C810) makes it Exited, bHasDeparted, when a team with a jewel gets in.
struct FHeistExitCraftData
{
    DefineUnrealStruct(FHeistExitCraftData, FortniteGame)
    DefineUProperty(AFortAthenaExitCraftSpawner*, ExitCraftSpawner)
    DefineUProperty(AFortAthenaExitCraft*, SpawnedExitCraft)
    DefineUProperty(EHeistExitCraftState, ExitCraftState)
    DefineUProperty(TArray<AFortPlayerPawnAthena*>, DepartedPawns)
    DefineUProperty(float, SpawnTime)
    DefineUProperty(bool, bHasDeparted)
};

// The Getaway's mutator (5.40, "Bling": Playlist_Bling_Solo, Duos and Squads, whose BlingModifierDefinition spawns
// Mutator_BlingMode_C; the playlists' WinConditionType is MutatorControlled). Its code is the build's own and intact:
// - BeginPlay (5.40 0xC44B20) reads the tags (SpecialActor.Bling.*, SpecialActor.Heist.ExitCraft, Loot.Type.Gadget.Bling)
//   and, on the authority, shuffles the vans' and the jewel drops' spawn data and binds the game state's
//   OnGamePhaseChanged / OnGamePhaseStepChanged;
// - the jewel drops (AthenaSupplyDrop_Bling_C, a safe with Loot_AthenaBlingDrop's Athena_Bling_Pack) are the playlist's
//   SupplyDropInfoList (BlingSupplyDrop: ItemDeliveryManagement, four jewels), which the game mode drops;
// - OnGamePhaseStepChanged starts the van timer (0xC47E10, the spawn data's SafeZonePhaseWhenToSpawn); Tick spawns the van
//   (0xC67CD0: AFortAthenaMapInfo picks the spot, an AFortAthenaExitCraftSpawner brings it in) and, once the van waits
//   for pawns, takes a team in when one of them carries the jewel (0xC4A3B0: the team places 1, AFortGameModeAthena's
//   0xC02B20 "Team %d placed %d!", ClientNotifyWon) and ends the match when the last van has left or nobody is left
//   (0xBECFF0).
// What it needs from the server and the build strips is the jewel dropping when its carrier goes down (the jewel's
// bDropOnDBNO; AFortPlayerControllerAthena::DropItemsOnDBNO). AFortGameModeAthena::WatchGetaway runs that and the log.
class AFortAthenaMutator_Heist : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_Heist, FortniteGame)

public:
    DefineUStructArrayProperty(FHeistExitCraftData, SpawnedExitCraftList)
    // When the next van spawns (the game state's server time; 0 while none is due), and which spawn data it uses.
    DefineUProperty(float, SpawnExitCraftTime)
    DefineUProperty(int32_t, CurrExitCraftIndexToSpawn)
    // The vans (replicated): still to come, in the match, and gone with a team.
    DefineUProperty(int32_t, NumUnspawnedExitCrafts)
    DefineUProperty(int32_t, NumSpawnedExitCrafts)
    DefineUProperty(int32_t, NumDepartedExitCrafts)
    // The alive players when each van left.
    DefineUProperty(TArray<int32_t>, NumPlayersAliveOnDeparture)
    // The jewels and the jewel drops (safes) the storm took (the post-match analytics).
    DefineUProperty(int32_t, JewelsLostToStorm)
    DefineUProperty(int32_t, SafesLostToStorm)
    // Loot.Type.Gadget.Bling: the jewel's item tag (Athena_Bling_Pack's GameplayTags).
    DefineUProperty(FGameplayTag, BlingLootItemTag)

    static bool HasHeistData() { return Member_SpawnedExitCraftList.IsAvailable() && Member_NumDepartedExitCrafts.IsAvailable() && Member_BlingLootItemTag.IsAvailable(); }
};
