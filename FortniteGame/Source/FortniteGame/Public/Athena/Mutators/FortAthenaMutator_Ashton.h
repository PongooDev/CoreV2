#pragma once

#include "Athena/Mutators/FortAthenaMutator.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"

class AFortPickup;
class AFortPlayerController;
class AFortPlayerControllerAthena;
class AFortPlayerStateAthena;
class UFortGadgetItemDefinition;
class UFortWorldItemDefinition;
class UWorld;

// One of the mutator's six stones as the match has it (StoneList, replicated): EAshtonStoneStateType NotSpawned 0,
// Spawned 1, Captured 2.
struct FAshtonStoneState
{
    DefineUnrealStruct(FAshtonStoneState, FortniteGame)
    DefineUProperty(uint8_t, StoneType)
    DefineUProperty(uint8_t, StoneState)
    DefineUProperty(bool, bHasEverSpawned)
};

// A stone's data (StoneDataList, six of them): its type and the special actor tag its supply drop and pickup are under.
struct FAshtonStoneData
{
    DefineUnrealStruct(FAshtonStoneData, FortniteGame)
    DefineUProperty(uint8_t, StoneType)
    DefineUProperty(FGameplayTag, GameplayTag)
};

// 8.50's Endgame (Playlist_Ashton_Sm, _Md, _Lg; AshtonMutator_C): heroes against Thanos and the Chitauri. The build keeps
// the mutator's stones (their supply drops, spawns and respawns), its villain respawns, its MutatorControlled win
// (FortGameModeAthena_CheckWinConditionOnPlayerRemoval, 0x105EEC0), the capture of a stone (its override of
// FortGameStateAthena_OnPickupPreGivenToPlayer, 0x10630C0, which AFortPickup::GivePickupTo now reaches through the
// controller's OnPickupPreGiven), the promotion of the next Thanos (the first stone's capture, or
// SelectNextVillainLeader's timer after the leader's death) and the team tags. Stripped in it are the gauntlet's give to
// the new leader and the removal of the leader's and the villains' items when they die; Core puts them back here, at the
// calls the build kept. The team loadouts are AshtonInventoryMutator's (an AFortAthenaMutator_InventoryOverride), which
// answer AFortGameModeZone::GetStartingItems.
class AFortAthenaMutator_Ashton : public AFortAthenaMutator
{
    DefineUnrealClass(AFortAthenaMutator_Ashton, FortniteGame)

public:
    DefineUProperty(FMulticastScriptDelegate, OnVillainRespawnsRemainingChanged)
    DefineUProperty(UFortGadgetItemDefinition*, VillainLeaderItemDef)
    DefineUProperty(TArray<UFortWorldItemDefinition*>, VillainItemDefs)
    DefineUProperty(AFortPlayerControllerAthena*, VillainLeaderPC)
    DefineUStructProperty(FAshtonStoneData, StoneDataList)
    DefineUStructArrayProperty(FAshtonStoneState, StoneList)
    static bool HasVillainMembers()
    {
        return StaticClass() && Member_OnVillainRespawnsRemainingChanged.IsAvailable() && Member_VillainLeaderItemDef.IsAvailable() && Member_VillainItemDefs.IsAvailable()
            && Member_VillainLeaderPC.IsAvailable();
    }

    // The match's Endgame mutator, among the game state's GameplayMutators; null outside Endgame and on builds without it.
    static AFortAthenaMutator_Ashton* GetCurrent(UWorld* World);

    // 21.40's unreflected VillainTeamNum, the byte after OnVillainRespawnsRemainingChanged (8.50 +0x2A8): the villains' team,
    // counted from the playlist's DefaultFirstTeam (the mutator's overrides compare TeamIndex - DefaultFirstTeam with it).
    uint8_t GetVillainTeamNum() const;
    // Whether the player is on the villains' side (the Chitauri and Thanos), as the mutator's own overrides tell it.
    bool IsVillain(const AFortPlayerStateAthena* PlayerState) const;

    // The stripped calls, where the build has them (Finder::ResolveAshtonMutatorLive), and the pickup's and the game mode's
    // calls the mode needs. Logged once at boot.
    static void HookNatives();

private:
    // The promotion's stripped give (0x1070F0F, after VillainLeaderPC is set): the new leader is given VillainLeaderItemDef,
    // the Infinity Gauntlet (AGID_CarminePack), whose gadget data (ApplyGadgetData, from the inventory's item-added
    // handling) makes him Thanos: AS_CarminePack's abilities, the XL body and the gauntlet's anim Blueprint. The call
    // passes only the controller; the mutator is the one whose VillainLeaderPC it now is.
    static void GiveVillainLeaderItemCall(AFortPlayerControllerAthena* NewVillainLeader);

    // OnPickupSpawnedAndReady(PickupObj, PickupItemDef), bound by BeginPlay (0x1056F60) to the game state's
    // OnPickupSpawnedAndReady, which AFortGameModePickup broadcasts on the server once it is set up (0xFEF070). The build
    // binds the shared empty function (lea at 0x10570B3), and the UFunction's exec (0x195C6C0) calls it too. 🟡 Inferred:
    // the build keeps its pair, OnPickupDestroying (0x106CB30), which finds the stone whose GameplayTag the item's
    // SpecialActorPickupTag is under and puts a stone that isn't captured back to NotSpawned; this marks that stone
    // Spawned (and bHasEverSpawned), so the HUD's stone list and the capture see it in play (CoreV2 #630, #631).
    void OnPickupSpawnedAndReady(AFortPickup* PickupObj, const UFortWorldItemDefinition* PickupItemDef);
    static void OnPickupSpawnedAndReadyHK(AFortAthenaMutator_Ashton* This, AFortPickup* PickupObj, const UFortWorldItemDefinition* PickupItemDef);
    DECLARE_FUNCTION(execOnPickupSpawnedAndReady);
};
