#pragma once

#include "UObject/Stack.h"

class AFortGameModeAthena;
class UObject;
class UWorld;

// 8.51's Unvaulting ("Snow", BP_SnowScripting in Athena_POI_Foundations, the pillars BP_DirtShell_Pillars in the Next2
// level) is played by Epic's mesh network: each match server is an Edge node, and one Root server holds the event's state
// for all of them and replicates it down the mesh (its own copy of the Blueprints runs their Root branches). Only the
// Edge branch is the match: BP_SnowScripting's BindCalendarEvents (GWS1 the lead-up, GWS0 the event, GWLevel Next2) runs
// only once UFortAsyncAction_MeshNetworkReady answers Edge, and then waits for what the Root sends:
//   - PillarsCompletedUTCTime 1001-01-01 08:00 ("the pillars aren't voted yet"), without which GWS0 waits forever after
//     the lead-up has made everyone invincible, unarmed and HUD-less;
//   - AllChallengesCompleted and ChallengeStates (the five pre-event challenges, done by 4 May 2019), for the countdown
//     and the pedestals;
//   - the pillar vote: an Edge sends each pickaxe hit (BP_DirtShellDamageComponent's DamagePropN, a NetServer function
//     UMeshNetworkComponent::ProcessEvent forwards to the parent node, 8.51 0x9384D0) and the Root's UpdateHealth lowers
//     PropHealth and picks VoteWinner;
//   - UnvaultedItemName, the winner (RootPillarsConcluded), which sets the matches' loot weights.
// A Core server has no mesh service and no Root (AFortGameSession::SetMeshNodeType, 8.51 0x153F1B0, joins one through the
// MCP mesh service and a parent's beacon), so it stands in for both on its one match: it is the Edge node, connected,
// and it does on its own Blueprints what the Root's replication and Root branches did.
class FCoreSnowMeshRoot
{
public:
    // From AFortGameModeZone::FinishWorldInitialization (the world is ready, the event's actors have begun play).
    // Nothing on builds without 8.51's BP_SnowScripting (8.50 has an earlier one without the event), in Creative, or where
    // the server already has a mesh node type.
    static void SetUp(AFortGameModeAthena* GameMode);
    // At boot (AFortGameModeAthena::HookVTables): the native Next2's OnLevelShown calls on the game mode.
    static void AddNatives();

private:
    // The Root's OnReady bindings on the pillars (BP_DirtShell's Root branch, and BP_SnowScripting's RootPillarsConcluded
    // on onPillarsConcludedParent), once Next2 is in.
    static void BindPillars(UWorld* World);
    // A pillar's pickaxe hit as the Root receives it: BP_DirtShell.UpdateHealth(Damage * DamageMultiplier, its index).
    static void HandlePillarDamage(UObject* Pillars, int32_t Damage, UObject* DirtProp);

    DECLARE_FUNCTION(execHandleNext2Shown);
    DECLARE_FUNCTION(execHandlePillarDamage);
};
