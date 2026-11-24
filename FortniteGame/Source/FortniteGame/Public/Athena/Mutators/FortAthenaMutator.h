#pragma once

#include "Containers/Array.h"
#include "FortGameModeZone.h"
#include "GameFramework/Info.h"
#include "GameplayEffect.h"
#include "Reflection.h"

#include <map>

class AFortGameStateAthena;
class AFortPawn;
class AFortPlayerController;
class AFortPlayerStateAthena;
class UFortItemDefinition;
class UFortWorldItemDefinition;

// Battle Royale's gameplay mutator (from 6.01): an AFortGameplayMutator in the game. Core declares no
// AFortGameplayMutator, so this derives from its AInfo. The playlist's modifiers spawn its subclasses. Those that change
// the game state's answers (bMutatesGameState) are in AFortGameStateAthena's GameplayMutators, and the game state's code
// asks each of them through its FortGameStateAthena_* virtuals.
class AFortAthenaMutator : public AInfo
{
    DefineUnrealClass(AFortAthenaMutator, FortniteGame)

public:
    // AFortGameplayMutator's (IsMutatorActive, 6.31 0x10FC7C0, reads it): the game state's loops over its mutators skip
    // one that isn't active (OnPlaylistTimerTicked's, 0xCAF640).
    DefineBitfieldUProperty(bMutatorActive)
    static bool HasMutatorActive() { return Member_bMutatorActive.IsAvailable(); }

    // 21.40's FortGameStateAthena_OnPostEarnedKill: a player earned a kill. 21.40 passes their player state; 6.31
    // passes their controller (Finder::ResolveOnPostEarnedKillLive checks that the override reads it as one).
    // AFortAthenaMutator's does nothing. 6.31's Team Rumble (AFortAthenaMutator_TDM, 0xD3C1B0) adds the kill to the
    // killer team's AFortTeamInfoAthena CurrentTeamScore and to the game state's MutatorGenericInt_0 or _1, the scores
    // its HUD shows. Nothing in the client build calls it: the server's call was stripped with the kill handling.
    // Server only; nothing on a build where the slot isn't found.
    static bool HasOnPostEarnedKill();
    void FortGameStateAthena_OnPostEarnedKill(AFortPlayerController* KillerController);
};

// Team Rumble's mutator (6.31's Playlist_Respawn_24, RespawnMutator_C). Its FortGameStateAthena_OnPostEarnedKill scores
// the teams. Its GetScoringModeWinningGoal (0xD3A0C0) gives the game mode's Tick the eliminations that win
// (ActualNumKillsForVictory, set at its BeginPlay from NumKillsForVictory).
class AFortAthenaMutator_TDM : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_TDM, FortniteGame)

public:
    // Whether it keeps the two teams' scores in the game state's MutatorGenericInt_0 and _1.
    DefineUProperty(bool, bIsTwoTeamTDM)

    // A kill by Core's player bot, which the override can't take. The override reads the earner's team from its
    // controller (AFortPlayerController's team member interface), and a bot's controller is a stock AAIController,
    // which has none. Epic's 6.31 has no AI players to ask for. So this does what 0xD3C1B0 does, with the bot's team
    // and its AFortTeamInfoAthena (the player state's PlayerTeam): one more on its CurrentTeamScore, copied with
    // bIsTwoTeamTDM to MutatorGenericInt_0 for HumanPvP_Team1 or _1 for the other team. Core's.
    void ScoreBotKill(AFortGameStateAthena* GameState, AFortPlayerStateAthena* KillerPlayerState);
};

// What an inventory override mutator says about one kind of item (from 6.01).
class EAthenaLootDropOverride
{
    DefineUnrealEnum(EAthenaLootDropOverride, FortniteGame)
    DefineUEnumValue(NoOverride)
    DefineUEnumValue(ForceDrop)
    DefineUEnumValue(ForceKeep)
};

// When an inventory override's loadout replaces a player's inventory (from 6.30).
class EAthenaInventorySpawnOverride
{
    DefineUnrealEnum(EAthenaInventorySpawnOverride, FortniteGame)
    DefineUEnumValue(NoOverride)
    DefineUEnumValue(Always)
    DefineUEnumValue(IntialSpawn)
    DefineUEnumValue(AircraftPhaseOnly)
};

// One of an inventory override mutator's loadouts.
struct FItemLoadoutContainer
{
    DefineUnrealStruct(FItemLoadoutContainer, FortniteGame)
    DefineUStructArrayProperty(FItemAndCount, Loadout)
};

// The loadout an inventory override mutator gives one team, and when.
struct FItemLoadoutTeamMap
{
    DefineUnrealStruct(FItemLoadoutTeamMap, FortniteGame)
    DefineUProperty(uint8_t, TeamIndex)
    DefineUProperty(uint8_t, LoadoutIndex)
    DefineUProperty(uint8_t, UpdateOverrideType)
    DefineUProperty(uint8_t, DropAllItemsOverride)
};

// The mutator that says what an eliminated player drops and keeps (from 6.30; 6.31's Team Rumble has one,
// RespawnInventoryDropMutator, with DropAllItemsOverride at ForceKeep). The game state's ShouldDropItem asks it through
// its FortGameStateAthena_ShouldDropItem (6.31 CL-4573279 0xD3C4C0, slot 0xE2), with the player's controller.
class AFortAthenaMutator_InventoryOverride : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_InventoryOverride, FortniteGame)

public:
    DefineUProperty(uint8_t, DropAllItemsOverride)
    DefineUProperty(uint8_t, TrapDropOverride)
    DefineUProperty(uint8_t, WeaponDropOverride)
    DefineUProperty(uint8_t, MaterialDropOverride)
    DefineUProperty(uint8_t, GadgetDropOverride)
    DefineUProperty(uint8_t, ConsumableDropOverride)
    DefineUProperty(uint8_t, AmmoDropOverride)
    static bool HasDropOverrides()
    {
        return Member_DropAllItemsOverride.IsAvailable() && Member_TrapDropOverride.IsAvailable() && Member_WeaponDropOverride.IsAvailable() && Member_MaterialDropOverride.IsAvailable()
            && Member_GadgetDropOverride.IsAvailable() && Member_ConsumableDropOverride.IsAvailable() && Member_AmmoDropOverride.IsAvailable();
    }

    // Whether an item of Core's player bot drops when the bot is eliminated (CoreV2 #537). The override needs the
    // player's controller, and a bot's is a stock AAIController, so this answers as 0xD3C4C0 does from the item's type:
    // false when the mutator has no say, else true with bOutShouldDrop its answer. Core's.
    bool ShouldDropBotItem(const UFortItemDefinition* ItemDefinition, bool& bOutShouldDrop) const;

    // The loadouts (CoreV2 #638): InventoryLoadouts[InventoryLoadoutIndex] for everyone, or a team's own from
    // TeamLoadouts, given when InventoryUpdateOverride (or the team's UpdateOverrideType) says.
    DefineUProperty(uint8_t, InventoryUpdateOverride)
    DefineUStructArrayProperty(FItemLoadoutContainer, InventoryLoadouts)
    DefineUProperty(int32_t, InventoryLoadoutIndex)
    DefineUStructArrayProperty(FItemLoadoutTeamMap, TeamLoadouts)
    static bool HasInventoryLoadouts()
    {
        return HasDropOverrides() && Member_InventoryUpdateOverride.IsAvailable() && Member_InventoryLoadouts.IsAvailable() && Member_InventoryLoadoutIndex.IsAvailable()
            && FItemLoadoutContainer::StaticStruct() && FItemAndCount::StaticStruct();
    }
    // TeamLoadouts is 8.30's (none in 6.30 to 8.20).
    static bool HasTeamLoadouts() { return Member_TeamLoadouts.IsAvailable() && FItemLoadoutTeamMap::StaticStruct(); }

    // 21.40's FortGameModeAthena_GetInventoryOverride (8.30 CL-5793395 0xFF5660, vtable slot 0x7E8; 8.50 CL-6058028
    // 0x108C490, slot 0x7D8): the loadout a player gets now, appended to OutItems, and true where the mutator answers (the
    // slot's 2, which ends the game's loop over the mutators). It takes the player's team from their controller's team
    // member interface, which a Core bot's controller isn't, so the team comes in here; the body is the slot's. Core's.
    bool GetInventoryOverrideForBot(bool bInitialSpawn, uint8_t TeamIndex, const AFortGameStateAthena* GameState, TArray<const FItemAndCount*>& OutItems) const;
};

// One of the items AFortAthenaMutator_ItemDropOnDeath leaves where a player died.
struct FItemsToDropOnDeath
{
    DefineUnrealStruct(FItemsToDropOnDeath, FortniteGame)
    DefineUProperty(UFortWorldItemDefinition*, ItemToDrop)
    DefineUStructProperty(FScalableFloat, NumberToDrop)
};

// What a player who comes back leaves where they died (from 6.31; Team Rumble's RespawnItemsToDropOnDeathMutator): a
// share of their ammo and ItemsToDrop (the 120 wood, 90 stone and 60 metal stacks). Its
// AFortGameModeAthena_OnPawnDeathBeforeRespawning (6.31 0xD30000) drops them, from the pawn's death (0xD09E00, which
// goes on only for a pawn an AFortPlayerControllerAthena controls).
class AFortAthenaMutator_ItemDropOnDeath : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_ItemDropOnDeath, FortniteGame)

public:
    DefineUStructProperty(FScalableFloat, PercentAmmoToDrop)
    DefineUStructProperty(FScalableFloat, MaxAmmoTypeDrops)
    DefineUStructProperty(FScalableFloat, MaxResourceTypeDrops)
    DefineUStructArrayProperty(FItemsToDropOnDeath, ItemsToDrop)
    static bool HasItemsToDrop()
    {
        return Member_PercentAmmoToDrop.IsAvailable() && Member_MaxAmmoTypeDrops.IsAvailable() && Member_MaxResourceTypeDrops.IsAvailable() && Member_ItemsToDrop.IsAvailable()
            && FItemsToDropOnDeath::StaticStruct();
    }

    // The same drop for Core's player bot (CoreV2 #537), whose pawn's death never reaches 0xD30000 (its controller is a
    // stock AAIController) and whose ammo isn't in an inventory the mutator can read: as 0xD30000 does, PercentAmmoToDrop
    // of each of the bot's ammo stacks (Ammo, what it holds of each), then ItemsToDrop's entries (up to
    // MaxAmmoTypeDrops of the ammo ones and MaxResourceTypeDrops of the resource ones, picked at random, every other one)
    // at NumberToDrop each, all spawned at the pawn. Core's.
    void DropBotItemsOnDeath(AFortPawn* DeadPawn, const std::map<UFortItemDefinition*, int32_t>& Ammo) const;
};

// A gravity mutator's movement values (FGravityMovementData; 0x14 bytes in every SDK from 6.01 to 8.00).
struct FGravityMovementData
{
    float GravityZScale = 1.0f;
    float VehicleGravityZScale = 1.0f;
    float JumpZVelocityOverride = 0.0f;
    float JumpHorizontalAccelerationOverride = 0.0f;
    float JumpHorizontalVelocityOverride = 0.0f;
};
static_assert(sizeof(FGravityMovementData) == 0x14);

// The playlist's low gravity (6.01 on; 7.30's One Shot, Playlist_Low_Squads: ModifierDefinition_Low's Mutator_Low_C,
// CustomGravityData { 0.377, 0.75, 896, 550, 1100 }). The game state asks it for the gravity a new pawn gets.
class AFortAthenaMutator_GravityMultiplier : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_GravityMultiplier, FortniteGame)

public:
    DefineUProperty(int32_t, GravityOverride)
    DefineUProperty(TArray<FGravityMovementData>, CustomGravityData)
    static bool IsAvailable() { return StaticClass() && Member_GravityOverride.IsAvailable() && Member_CustomGravityData.IsAvailable(); }

    // Its answer to the game state's gravity question (7.30 CL-4834550 0xDD0DE0, its virtual at +0x7A8): the
    // CustomGravityData entry GravityOverride picks, none when the index is out of range.
    bool GetGravityData(FGravityMovementData& OutData) const;
};
