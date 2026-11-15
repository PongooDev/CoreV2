#pragma once

#include "Athena/AthenaBarrierObjective.h"
#include "Athena/FortGameStateAthena.h"
#include "Engine/EngineTypes.h"
#include "FortTeamInfo.h"
#include "GameFramework/Info.h"
#include "GameplayEffect.h"

#include <vector>

class AAthenaBarrierFlag;
class AAthenaBigBaseWall;
class AFortGameModeAthena;
class UClass;
class UCustomCharacterPart;
class UWorld;

// One of Food Fight's two teams, as the Barrier mutator keeps it (Team_0_State, Team_1_State): its team, its food
// (the constructor's MAX until BeginPlay's SetupTeamStates), its restaurant and mascot, and whether its players come back. The
// build's own natives read it: IsRespawningAllowed's answer and the death drop go by bRespawnEnabled (6.30 0xD36E40,
// 0xD2CBC0), the respawn spot by TeamNum (0xD33050), the HUD messages by FoodTeam.
struct FBarrierTeamState
{
    DefineUnrealStruct(FBarrierTeamState, FortniteGame)
    DefineUProperty(EFortTeam, TeamNum)
    DefineUProperty(EBarrierFoodTeam, FoodTeam)
    DefineUProperty(AAthenaBarrierFlag*, ObjectiveFlag)
    DefineUProperty(AAthenaBarrierObjective*, ObjectiveObject)
    DefineUProperty(bool, bRespawnEnabled)

    // The unreflected FTimerHandle in its last 8 bytes (6.30: Team_0_State +0x20): the team's CheckHealthThreshold
    // timer, which CheckHealthThreshold itself clears once the mascot is in its last damage state (0xD30690:
    // ClearTimer on mutator +0x408 / +0x430).
    FTimerHandle& GetHealthCheckTimerHandle();
};

// A food's look on its team's players (FoodHeadData, one per EBarrierFoodTeam): Mutator_Barrier_C's burger
// (CP_Hat_M_Commando_FF_Burger) and tomato (CP_Hat_M_Commando_FF_Tomato) hats.
struct FBarrierHeadData
{
    DefineUnrealStruct(FBarrierHeadData, FortniteGame)
    DefineUProperty(TArray<UCustomCharacterPart*>, PartsToSwapIn)
};

// Food Fight's mutator (from 6.30; Mutator_Barrier_C, Playlist_Barrier's BarrierModifierDefinition). Core declares no
// AFortAthenaMutator, so this derives from its AInfo.
//
// What the build keeps of it: BeginPlay (0xD2E760) sets bMutatorControlledWin for a MutatorControlled playlist, gives
// the team states their teams and food (21.40's SetupTeamStates, inlined: the playlist's DefaultFirstTeam the burger,
// the next team the tomato) and binds OnGamePhaseStepChanged (0xD46DC0) to the game state's GamePhaseStepChanged on the
// authority, which sends the intro message at BusLocked, starts the mascots' destruction timer, starts the wall's fall
// and turns the respawns off at their storm circles. The game mode's tick asks the mutator
// (slot 0xC1, 0xD349C0) whether a team has won, and places it first and ends the match when WinningTeam is set. The
// answers the game asks for (IsRespawningAllowed, the respawn spot, the death drop, the team's head) are its overrides.
//
// What the build doesn't have is the rest of its server code: spawning the wall and the two restaurants (and turning
// the teams' respawns on with them), and the mutator's side of the wall's and the mascots' native delegates
// (OnObjectiveDestroyed, OnWallIsDown). Those are here. OnObjectiveDestroyed, OnWallIsDown and
// FortGameStateAthena_GatherCustomCharacterPartOverrides are 21.40's names; the spawning functions' are Core's.
class AFortAthenaMutator_Barrier : public AInfo
{
    DefineUnrealClass(AFortAthenaMutator_Barrier, FortniteGame)
    DefineUFunction(CheckHealthThreshold)

public:
    DefineUProperty(UClass*, BigBaseWallClass)
    DefineUProperty(UClass*, ObjectiveFlag)
    DefineUStructProperty(FBarrierHeadData, FoodHeadData)
    DefineUProperty(AAthenaBigBaseWall*, BigBaseWall)
    DefineUStructProperty(FBarrierTeamState, Team_0_State)
    DefineUStructProperty(FBarrierTeamState, Team_1_State)
    DefineUStructProperty(FScalableFloat, ObjectiveDistanceFromWall)
    DefineUStructProperty(FScalableFloat, ObjectiveZOffset)
    DefineUStructProperty(FScalableFloat, TimeToBringDownWall)
    DefineUStructProperty(FScalableFloat, SafeZonePhaseWhenToBringDownWall)
    DefineUStructProperty(FScalableFloat, PercentAmmoToDrop)

    // The server's part of the mutator's OnGamePhaseStepChanged, from the game state's GamePhaseStepChanged (Core's
    // AFortGameModeAthena::HandleGamePhaseStepChanged hears the same broadcast and passes the step on): the wall and the
    // restaurants at BusLocked, the step the build's own sends GameMsg_Intro at ("The enemy team is preparing to attack
    // your restaurant. Build a fort around it!"), once the buses' paths are chosen and before their drop.
    void OnGamePhaseStepChanged(AFortGameModeAthena* GameMode, uint8_t GamePhaseStep);

    // 21.40's FortGameStateAthena_GatherCustomCharacterPartOverrides: the parts a member of TeamNum wears over their
    // outfit's, its food's PartsToSwapIn (none before SetupTeamStates gave the teams their food). The build's own (6.30
    // 0xD36460, slot 0xF2) takes the player controller and reads the team from it: AFortPlayerState's
    // ApplyCharacterCustomization (0x133D8A0) asks the game state's mutators (its GatherCustomCharacterPartOverrides,
    // slot 0x106, 0xCA0000) only for a player state a player controller owns, so a player bot's parts never had them.
    // Core asks this one by the team for a bot.
    void FortGameStateAthena_GatherCustomCharacterPartOverrides(uint8_t TeamNum, std::vector<UCustomCharacterPart*>& OutParts) const;
    // The same from every Barrier mutator among the game state's GameplayMutators (none outside Food Fight).
    static void GatherCustomCharacterPartOverrides(UWorld* World, uint8_t TeamNum, std::vector<UCustomCharacterPart*>& OutParts);

    // The Barrier mutators among the game mode's GameplayMutators.
    static void ForEachBarrierMutator(AFortGameModeAthena* GameMode, void (*Visit)(AFortAthenaMutator_Barrier*, AFortGameModeAthena*, uint8_t), uint8_t GamePhaseStep);

private:
    // The wall along the game state's FlightPathMidLine, on the ground at its middle (AAthenaBigBaseWall::CreateWall),
    // and the mutator's side of its two delegates.
    void SpawnBigBaseWall(AFortGameModeAthena* GameMode);
    // Each team's restaurant (ObjectiveFlag) ObjectiveDistanceFromWall off the wall's middle, on the side its bus flies,
    // facing the wall; its mascot's destroyed delegate, and the team's CheckHealthThreshold timer.
    void SpawnObjectives(AFortGameModeAthena* GameMode);
    void SpawnObjective(FBarrierTeamState& TeamState, const FVector& Location, float Yaw);
    // The build's CheckHealthThreshold(TeamNum) (0xD30510): the mascot's damage state from its health against
    // ObjectiveHealthTiers, and the game message of a new one.
    void CheckHealthThreshold(uint8_t TeamNum);
    // 21.40's OnObjectiveDestroyed: a team's mascot is gone. That team respawns no more, the other wins
    // (WinningTeam, which the mutator's own game mode tick then places and ends the match with), and every client is
    // told (EventId 3, the losing team: "Your team's mascot has been destroyed -- game over!" for it).
    void OnObjectiveDestroyed(AAthenaBarrierObjective* Objective);
    // The wall starts to fall: GameMsg_WallComingDown (EventId 1).
    void OnWallStartingToComeDown();
    // 21.40's OnWallIsDown: GameMsg_WallDown (EventId 2), and the mascots can be damaged from now on.
    void OnWallIsDown();
    void SendMutatorEvent(int32_t EventId, int32_t EventParam1);

    // The unreflected WinningTeam (6.30 +0x384, 103 from the constructor until a team has won), from the reflected
    // ObjectiveFlag before it (+0x378). Null where the value there isn't the constructor's or a team's.
    int32_t* GetWinningTeam() const;
};
