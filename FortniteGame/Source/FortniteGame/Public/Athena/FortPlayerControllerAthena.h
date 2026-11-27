#pragma once

#include "UObject/AssetPtr.h"
#include "Containers/Array.h"
#include "Internationalization/Text.h"
#include "Items/FortItemDefinition.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerControllerZone.h"
#include "UObject/WeakObjectPtr.h"

#include <functional>
#include <vector>

class AActor;
class AController;
class AFortAthenaCreativePortal;
class AFortPlayerState;
class UFortWorldItemDefinition;
class AFortVolume;
class UFortCreativeRealEstatePlotItem;
class UMcpProfile;
class APawn;
class AFortPawn;
class AFortPlayerPawn;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class APhysicsObject;
class UAthenaMarkerComponent;
class UAthenaPlayerMatchReport;
class UFortHero;
class UFortHeroType;
class UFortWeaponItemDefinition;
struct FAthenaMatchStats;
struct FAthenaMatchTeamStats;
struct FAthenaRewardResult;
struct FGameplayTagContainer;
struct FFortItemEntry;
struct FGuid;
struct FMcpQueryResult;
struct FTimeStampedPhysicsPawnState;

// One of a player's Creative islands as the controller replicates it (from 7.00): its name and its plot item's id.
struct FCreativeIslandData
{
    DefineUnrealStruct(FCreativeIslandData, FortniteGame)

public:
    DefineUProperty(FText, IslandName)
    DefineUProperty(FString, McpId)
};

// What a player publishes their island with (7.10's island menu, Publish Island: UFortCreativePublishModal):
// FCreativePublishOptions, which PublishMyCurrentCreativePlot (7.10 to 7.30, 0x30 bytes) and PublishCreativePlot
// (7.40, 0x40 bytes: UserYoutubeVideoId before UserLocale) take by value. Read through reflection, as its layout moves.
struct FCreativePublishOptions
{
    DefineUnrealStruct(FCreativePublishOptions, FortniteGame)
    DefineUProperty(FString, UserTitle)
    DefineUProperty(FString, UserDescription)
    DefineUProperty(FString, UserLocale)
};

// An island's name and description as the island menu's Description tab sends them (7.40, CL-5046157:
// UFortCreativeMoreInfoPanel's Apply), which UpdateCreativePlotData takes by value.
struct FCreativeIslandInfo
{
    DefineUnrealStruct(FCreativeIslandInfo, FortniteGame)
    DefineUProperty(FString, IslandTitle)
    DefineUProperty(FString, IslandIntroduction)
    DefineUProperty(FString, UserLocale)
};

// A Battle Royale outfit (from 1.8; CID_*): the hero it plays (HID_*_Athena_Commando_*).
class UAthenaCharacterItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UAthenaCharacterItemDefinition, FortniteGame)

public:
    DefineUProperty(UFortHeroType*, HeroDefinition)
};

// A Battle Royale pickaxe (from 1.8; DefaultPickaxe, Pickaxe_ID_*): the harvesting tool it gives.
class UAthenaPickaxeItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UAthenaPickaxeItemDefinition, FortniteGame)

public:
    DefineUProperty(UFortWeaponItemDefinition*, WeaponDefinition)
};

// A Battle Royale glider (from 1.8; DefaultGlider, Glider_ID_*).
class UAthenaGliderItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UAthenaGliderItemDefinition, FortniteGame)
};

// The cosmetics a player wears in Battle Royale (from 1.8), from their athena profile's favorite_* items.
struct FFortAthenaLoadout
{
    DefineUnrealStruct(FFortAthenaLoadout, FortniteGame)
    // The athena profile's banner_icon and banner_color.
    DefineUProperty(FString, BannerIconId)
    DefineUProperty(FString, BannerColorId)
    DefineUProperty(UAthenaGliderItemDefinition*, Glider)
    DefineUProperty(UAthenaPickaxeItemDefinition*, Pickaxe)
    DefineUProperty(UAthenaCharacterItemDefinition*, Character)
    // The contrail and back bling (not on every build's loadout).
    DefineUProperty(UObject*, SkyDiveContrail)
    DefineUProperty(UObject*, Backpack)
    // The locker's emote slots (UAthenaDanceItemDefinition*); the strong references that keep them loaded on the server.
    DefineUProperty(TArray<UObject*>, Dances)

    static bool HasGlider() { return Member_Glider.IsAvailable(); }
    static bool HasDances() { return Member_Dances.IsAvailable(); }
    static bool HasSkyDiveContrail() { return Member_SkyDiveContrail.IsAvailable(); }
    static bool HasBackpack() { return Member_Backpack.IsAvailable(); }
};

// One callout of a Squad Comms bank (from 3.5): its texts, icon, the emoji the sender plays (a
// UAthenaEmojiItemDefinition: 3.5.1's are all Emoji_Comm, the callout bubble) and the ETeamMemberState it shows the squad.
struct FAthenaQuickChatLeafEntry
{
    DefineUnrealStruct(FAthenaQuickChatLeafEntry, FortniteGame)
    DefineUProperty(UFortMontageItemDefinitionBase*, EmojiItemDefinition)
    DefineUProperty(uint8_t, TeamCommType)
};

// The Squad Comms wheel's callouts (from 3.3; AthenaQuickChatBank assets, the UI's SquadChatOptionBanks).
class UAthenaQuickChatBank : public UObject
{
    DefineUnrealClass(UAthenaQuickChatBank, FortniteGame)

public:
    DefineUStructArrayProperty(FAthenaQuickChatLeafEntry, ChatOptions)
};

// The callout a player picked on the wheel (from 3.3): its bank and its index in the bank's ChatOptions, with what it
// is about (the item or material it asks for). 5.21 (CL-4288479) makes Bank and ContextObject TWeakObjectPtrs, as 21.40
// has them; 3.3 to 5.10 reflect raw object pointers in the same 8 bytes. GetBank and GetContextObject read either.
struct FAthenaQuickChatActiveEntry
{
    DefineUnrealStruct(FAthenaQuickChatActiveEntry, FortniteGame)
    DefineUProperty(TWeakObjectPtr<UAthenaQuickChatBank>, Bank)
    DefineUProperty(TWeakObjectPtr<UObject>, ContextObject)
    DefineUProperty(int16_t, ContextValue)
    DefineUProperty(int8_t, Index)

    UAthenaQuickChatBank* GetBank() const;
    UObject* GetContextObject() const;
};

// Battle Royale's player controller (from 1.7.2; Athena_PlayerController_C).
class AFortPlayerControllerAthena : public AFortPlayerController
{
    DefineUnrealClass(AFortPlayerControllerAthena, FortniteGame)

public:
    // The heroes 1.7.2's Battle Royale plays (Athena_PlayerController_C: HID_Commando_Athena_*). 1.8 leaves it empty,
    // and 2.2 (CL-3841827) no longer has it: read it through HasDefaultHeroes.
    DefineUProperty(TArray<UFortHeroType*>, DefaultHeroes)
    // 1.8: the player's loadout. Athena_PlayerController_C's default has DefaultGlider and DefaultPickaxe in it.
    DefineUStructProperty(FFortAthenaLoadout, CustomizationLoadout)
    // 8.20 (CL-5625478) moves it to AFortPlayerController as CosmeticLoadoutPC (21.40's name); declared here, where
    // FFortAthenaLoadout is, and found on the controller through the class chain.
    DefineUStructProperty(FFortAthenaLoadout, CosmeticLoadoutPC)
    static bool HasCosmeticLoadout() { return Member_CustomizationLoadout.IsAvailable() || Member_CosmeticLoadoutPC.IsAvailable(); }
    // The player's loadout, whichever the build has; null where it has neither.
    FFortAthenaLoadout* GetCosmeticLoadout() const
    {
        return Member_CustomizationLoadout.IsAvailable() ? &static_cast<FFortAthenaLoadout&>(CustomizationLoadout)
            : Member_CosmeticLoadoutPC.IsAvailable() ? &static_cast<FFortAthenaLoadout&>(CosmeticLoadoutPC) : nullptr;
    }
    // The player's team. Server: filled once, while empty, by the controller's fill (1.9 slot 0x32E, 1.11 and 2.1 0x338:
    // the game state's and the game mode's inactive player states on the controller's team, 50v50's whole 50), which the
    // team's place and the wins read. Client up to 1.10: the HUD's team list takes their talking states in this order
    // (GetTeamTalking, 1.7.2 0x430440, and the controller's tick, 0x417630, which tells the HUD when they change).
    DefineUProperty(TArray<AFortPlayerStateAthena*>, TeamMembers)
    // 1.11: the player's squad (AFortPlayerStateAthena::SquadId) within the team, which the HUD's team list follows
    // instead (GetSquadTalking, 0x43BC20, and the controller's tick read their talking states from it). The client adds
    // to it as squads replicate (OnRep_SquadId); the server fills it as it does TeamMembers (slot 0x339).
    DefineUProperty(TArray<AFortPlayerStateAthena*>, SquadMembers)
    // Server: the player is in the game mode's AlivePlayers (AddToAlivePlayers sets it, RemoveFromAlivePlayers clears it).
    DefineUProperty(bool, bMarkedAlive)
    // Server: who SpectateOnDeath watches first after a death, the eliminator's pawn.
    DefineUProperty(APawn*, PlayerToSpectateOnDeath)
    // 8.00 (CL-5203069): the player's map and world markers (UAthenaMarkerComponent, new in 8.00), which the pawn-death
    // handler asks to take the player's map marker away (AFortPlayerControllerAthena 0xDBDEF0: [this+0x2DE8], 0xE2FB30).
    DefineUProperty(UAthenaMarkerComponent*, MarkerComponent)
    DefineUFunction(SpectateOnDeath)
    // 7.30 to 8.00 (21.40 has it on AFortPlayerController): the player is holding interact to swap their held item for a
    // pickup (ServerSetShouldSwapPickup sets it; its _Implementation is intact). Older builds don't have it.
    DefineUProperty(bool, bTryPickupSwap)
    static bool HasTryPickupSwap() { return Member_bTryPickupSwap.IsAvailable(); }
    // The build's SpectateOnDeath (1.9 0x438550, 3.3 0xA31650, 4.5 0xAAC400, 5.40 0xC34710), through its UFunction: what
    // the death's spectate timer calls (OnPawnDied).
    void SpectateOnDeath();
    // Server: a player who is back in play (a respawn, or a warmup restart) stops spectating after their death: the
    // death's spectate timer goes and the player state stops watching anyone (Core's: the game's RespawnPlayerAfterDeath,
    // 4.5 0xAA8410, leaves both alone, and SpectateOnDeath only returns early for a view target that is alive).
    void StopSpectatingOnDeath();
    // FortPlayerControllerAthenaDeath.cpp: the earned elimination's Event.EarnedElimination on the eliminator's pawn
    // (GAS_DefaultPlayer's GA_SiphonEffect_OnKillGrant triggers on it), from OnPawnDied and a bot's death.
    static void SendEarnedEliminationEvent(AFortPlayerStateAthena* KillerPlayerState, AFortPawn* EliminatedPawn);
    // AFortPlayerControllerZone's: the report of the player's last death, which ClientOnPawnDied sends.
    DefineUStructProperty(FFortPlayerDeathReport, PlayerDeathReport)
    // 1.10: the player's report of their match (AthenaPlayerMatchReport.h), made in BeginPlay on the server too.
    DefineUProperty(UAthenaPlayerMatchReport*, MatchReport)
    // The end-of-match screen's stats (FAthenaMatchStats) and its rewards, which end its "Team Still Alive" wait.
    DefineUFunction(ClientSendMatchStatsForPlayer)
    // 1.11: the screen's place and total players, out of FAthenaMatchStats.
    DefineUFunction(ClientSendTeamStatsForPlayer)
    DefineUFunction(ClientSendEndBattleRoyaleMatchForPlayer)
    // The winning team's notice (3.0 on): the game's team placement (5.10 0xBDCD70, "Team %d placed %d!", from
    // CheckWinConditionLastManStanding 0xBB69B0) sends it to each player of the team, alive or out, with place 1. From
    // 4.4 it takes the finisher's pawn, its weapon and the death cause: its _Implementation (5.10 0xBF1560) plays
    // PlayWinEffects with a finisher's pawn, and GameState->OnWinnerAnnounced (the victory slow motion) when that pawn
    // is the player's own.
    DefineUFunction(ClientNotifyTeamWon)

    static bool HasMatchReport() { return Member_MatchReport.IsAvailable(); }
    static bool HasSquadMembers() { return Member_SquadMembers.IsAvailable(); }
    UAthenaPlayerMatchReport* GetMatchReport();
    void ClientSendMatchStatsForPlayer(const FAthenaMatchStats& Stats);
    void ClientSendTeamStatsForPlayer(const FAthenaMatchTeamStats& TeamStats);
    // Result null: no rewards (MCP's EndBattleRoyaleGame, which answered with them, has no caller left).
    void ClientSendEndBattleRoyaleMatchForPlayer(bool bSuccess, const FAthenaRewardResult* Result);
    // Whether the build's ClientNotifyTeamWon takes the finisher (4.4 on).
    static bool HasClientNotifyTeamWonFinisher();
    void ClientNotifyTeamWon(APawn* FinisherPawn, const UFortWeaponItemDefinition* FinishingWeapon, uint8_t DeathCause);

    // Whether the controller plays Battle Royale heroes only: 1.7.2's DefaultHeroes, or 1.8's loadout's.
    // Whether the build has DefaultHeroes and the controller holds any (1.7.2).
    bool HasDefaultHeroes() const { return Member_DefaultHeroes.IsAvailable() && DefaultHeroes.Num() > 0; }
    bool PlaysBattleRoyaleHeroes();
    bool IsDefaultHero(UFortHero* Hero);
    // 1.8: the hero of the loadout's character, once GetOrInitializeHero has made the loadout.
    UFortHeroType* GetLoadoutHero();
    // 1.8: the harvesting tool of the loadout's pickaxe, or of the class's default loadout's (DefaultPickaxe). 1.7.2
    // has none: its pickaxe is one of the game mode's StartingItems.
    UFortWorldItemDefinition* GetLoadoutPickaxe();
    // 1.8: puts the class's default loadout's glider and pickaxe where the loadout, and Pawn's copy of it, have none.
    void FillEmptyLoadout(AFortPlayerPawnAthena* Pawn);
    // Core's: the emotes the controller's and the pawn's loadouts hold (#547).
    void LogLoadoutEmotes(AFortPlayerPawnAthena* Pawn);
    UFortHero* GetOrInitializeHero();
    UFortHero* SetupHero();
    void SetupBackpackSize();
    void GiveLoadoutPickaxe();
    // Takes back what the player picked up in warmup: the inventory is the game mode's StartingItems again.
    void ResetToStartingItems();

    // An eliminated player's inventory lies around where they died: everything that can be dropped, one pickup each.
    void DropInventoryOnDeath(AFortPawn* DeadPawn);
    // The spots on the floor around DeathLocation that DropInventoryOnDeath tosses Count pickups to (DeadPawn is the
    // traces' world context and is ignored by them).
    static std::vector<FVector> GetInventoryDropSpots(AActor* DeadPawn, const FVector& DeathLocation, int32_t Count);
    // Server: the controller of a player who just died stands over their body, and a couple of frames later, once the
    // game's own death handling is done, their client's does too (the minimap follows it, #139). The camera stays the
    // game's death camera (#125).
    void PlaceAtDeathLocation(AFortPawn* DeadPawn);
    // PlaceAtDeathLocation's timer, a couple of frames after the death: tells the client its controller stands at
    // ViewLocation (ClientSetSpectatorCamera) once the controller has let DeadPawn go and views itself.
    void SendDeathView(TWeakObjectPtr<AFortPawn> DeadPawn, FVector ViewLocation);

    // Squad Comms (from 3.3): the wheel sends ServerPlaySquadQuickChatMessage (its exec, 3.5.1 0x1192190, calls
    // _Validate, slot 0x364, then _Implementation, slot 0x365); ClientReceiveSquadQuickChatMessage is the squad's copy.
    DefineUFunction(ServerPlaySquadQuickChatMessage)
    DefineUFunction(ClientReceiveSquadQuickChatMessage)
    void ServerPlaySquadQuickChatMessage_Implementation(const FAthenaQuickChatActiveEntry& ChatEntry);
    void ClientReceiveSquadQuickChatMessage(const FAthenaQuickChatActiveEntry& ChatEntry, AFortPlayerController* SenderPC);
    // Where the build strips ServerPlaySquadQuickChatMessage_Implementation (3.5.1: its body only destroys SenderID).
    static void HookQuickChatVTables();

    // RPCs that only fill the sender's own player state: a squadmate the player's client calls a friend (from 3.0,
    // AFortPlayerStateAthena::FriendsInSquad) and the player's Anonymous Mode setting (from 7.30, bUsingAnonymousMode).
    DefineUFunction(ServerSendSquadFriend)
    DefineUFunction(ServerEnableAnonymousMode)
    void ServerSendSquadFriend_Implementation(AFortPlayerStateAthena* FriendInSquad);
    void ServerEnableAnonymousMode_Implementation();
    // Where the build strips them: their _Implementation slots hold the shared empty function (7.30's ServerSendSquadFriend
    // 0x41B and ServerEnableAnonymousMode 0x419, in every Athena controller class).
    static void HookPlayerStateRpcVTables();

    // Creative (from 7.00; FortPlayerControllerAthenaCreative.cpp). The player's islands, replicated from their creative
    // profile, the last one they used, their rift of the hub and the island space it is linked to.
    DefineUStructArrayProperty(FCreativeIslandData, CreativeIslands)
    DefineUProperty(FString, LastUsedCreativeIsland)
    DefineUProperty(AFortVolume*, CreativePlotLinkedVolume)
    DefineUProperty(AFortAthenaCreativePortal*, OwnedPortal)
    static bool HasOwnedPortal() { return Member_OwnedPortal.IsAvailable() && Member_CreativePlotLinkedVolume.IsAvailable(); }
    // What tells the player their island edit rights may have changed (from 7.00): Athena_PlayerController binds its
    // PermissionsChanged to it at BeginPlay in a Creative playlist, which runs HandlePermissions(GetCurrentVolume()): the
    // Creative phone (WID_CreativeTool) given to a player the island's UFortLevelSaveComponent lets edit, taken from one
    // it doesn't. The game broadcasts it from the controller's OnMinigameVolumeStateChanged (7.30 0xD70510) and, on 7.10
    // and 7.20, from the end of a permissions change (AFortPlayerStateAthena's MCP completion, 7.20 0xD89D50); from 7.30
    // Core's UFortLevelSaveComponent::OnModifyCreativePlotPermissionsComplete does that part.
    DefineUProperty(FMulticastScriptDelegate, OnCreativePermissionsChanged)
    static bool HasCreativePermissionsChanged() { return Member_OnCreativePermissionsChanged.IsAvailable(); }
    // 8.00 and 8.20: (Pickup, ItemDef, ItemTags) for each item the player drops that carries Athena.NotifyPickupSpawned
    // (AGID_Athena_Booty). GA_Athena_Booty_Active binds it: the map dropped after its chest was searched is destroyed.
    DefineUProperty(FMulticastScriptDelegate, OnItemDropSpawned)
    static bool HasItemDropSpawned() { return Member_OnItemDropSpawned.IsAvailable(); }
    DefineUFunction(ServerGiveCreativeItem)
    DefineUFunction(ServerLoadPlotForPortal)
    DefineUFunction(ServerReloadCreativePlot)
    DefineUFunction(ServerStopSavingCreativePlot)
    DefineUFunction(ResetMyCurrentCreativePlot)
    DefineUFunction(ServerTeleportToPlaygroundIslandDock)
    DefineUFunction(ServerTeleportToPlaygroundLobbyIsland)
    DefineUFunction(ClientCycleQuickBarToCreativeItem)

    AFortAthenaCreativePortal* GetOwnedPortal() const { return HasOwnedPortal() ? static_cast<AFortAthenaCreativePortal*>(OwnedPortal) : nullptr; }
    void SetOwnedPortal(AFortAthenaCreativePortal* Portal);
    // The island the player is on (CreativePlotLinkedVolume, null in the hub), through the game's own setter
    // (Natives::AFortPlayerControllerAthena_SetCreativePlotLinkedVolume): it binds the controller's
    // OnMinigameVolumeStateChanged to the island's state and refreshes the old island's rifts. The new island's rifts are
    // refreshed after it, so their CurrentPopulation counts the player. Assigns the property where the setter wasn't found.
    // Does nothing when Volume is already the link: the setter would unbind the controller from that same island.
    void SetCreativePlotLinkedVolume(AFortVolume* Volume);
    // The player's own island: their rift's (null without a rift).
    AFortVolume* GetOwnedIsland() const;
    // Restored _Implementations (7.00's are the shared empty function: slots 0x419, 0x437, 0x439, 0x43D, 0x43F, 0x441).
    void ServerGiveCreativeItem_Implementation(const FFortItemEntry& CreativeItem);
    void ServerReloadCreativePlot_Implementation();
    void ServerStopSavingCreativePlot_Implementation();
    void ResetMyCurrentCreativePlot_Implementation();
    void ServerTeleportToPlaygroundIslandDock_Implementation();
    void ServerTeleportToPlaygroundLobbyIsland_Implementation();

    // Creative's travel screen (PongooDev/CoreV2#497): a trip from a rift to its island, or from an island back to the
    // hub, goes through the respawn preparation the game uses for a respawn. The server stores the destination in the
    // player state's RespawnData (ServerSetRespawnData), sends ClientStartRespawnPreparation with a camera behaviour row
    // of RespawnCameraData (CreativePortal, TeleportToLobbyIsland) and, from 7.10, the HUD's text (ClientIslandTravelText
    // "Traveling to Island...", ClientTravelToCreativeHubText "Traveling to Creative Hub..."). The client blacks the
    // screen out, streams the place in and says it is ready (ServerClientIsReadyToRespawn, the build's own); the server
    // then moves the pawn. bDelayedTeleporting holds the rift's Blueprint off the player meanwhile (BGA_IslandPortal
    // returns at once for a controller that has it), and RespawnPlayerAfterDeath clears it (7.20 0xD60080).
    DefineUProperty(bool, bDelayedTeleporting)
    DefineUProperty(FText, ClientIslandTravelText)
    DefineUProperty(FText, ClientTravelToCreativeHubText)
    DefineUFunction(ClientStartRespawnPreparation)
    // RespawnPlayerAfterDeath(bool bEnterSkydiving) (8.30 0xF7E020): waits for a stored RespawnData to be ready on both
    // sides, then ServerRestartPlayer, the skydive, and DeathInfo cleared.
    DefineUFunction(RespawnPlayerAfterDeath)
    DefineUFunction(ServerSetRespawnData)
    static bool HasDelayedTeleport();
    bool IsDelayedTeleporting() const { return Member_bDelayedTeleporting.IsAvailable() && static_cast<bool>(bDelayedTeleporting); }
    // The HUD's texts (from 7.10), null where the build has none.
    const FText* GetClientIslandTravelText() const { return Member_ClientIslandTravelText.IsAvailable() ? &GetMember_ClientIslandTravelText() : nullptr; }
    const FText* GetClientTravelToCreativeHubText() const { return Member_ClientTravelToCreativeHubText.IsAvailable() ? &GetMember_ClientTravelToCreativeHubText() : nullptr; }
    // The client RPC (7.00 has no HUDReasonText: HUDReasonText is left out there).
    void ClientStartRespawnPreparation(const FVector& RespawnLoc, const FRotator& RespawnRot, float RespawnCameraDist, FName InRespawnCameraBehavior, const FText* HUDReasonText);
    // The start of a delayed teleport to Location, as AFortAthenaCreativePortal::TeleportVehicle does it for a vehicle's
    // players (7.20 0xCB26E0): the destination into RespawnData, ClientStartRespawnPreparation, bDelayedTeleporting.
    // False where the build lacks any of it (nothing is sent then, and the caller teleports at once).
    bool StartDelayedTeleport(const FVector& Location, const FRotator& Rotation, const wchar_t* CameraBehavior, const FText* HUDReasonText);
    // Once both sides are ready, OnReady runs with the stored destination; then RespawnData and bDelayedTeleporting are
    // cleared, and the client's camera leaves its respawn camera actor for the pawn (ClientSetViewTarget).
    // TeleportVehicle's timer (7.20 0xC9C210) checks every 0.1 s (FTimerManager), sets bServerIsReady and waits for
    // bClientIsReady.
    void FinishDelayedTeleportWhenReady(std::function<void(AFortPlayerControllerAthena*, const FVector&, const FRotator&)> OnReady, int32_t Checks = 0);
    // 7.20's natives for the trip back to the hub (both execs call the shared empty function; 7.00 and 7.10 don't have
    // them). PreLobbyIslandTeleport shows the travel screen for the hub start stored in RespawnData; PostLobbyIslandTeleport
    // moves PlayerPawn there once the client is ready, and the player is off their island.
    void PreLobbyIslandTeleport();
    void PostLobbyIslandTeleport(AFortPlayerPawn* PlayerPawn);
    // 7.10's and 7.20's native (its exec is the shared empty one; 7.00 doesn't have it), restored (#499): the player
    // loses every item that can be dropped (UFortWorldItemDefinition::bCanBeDropped) and isn't one of the game mode's
    // StartingItems. What can't be dropped stays: the pickaxe, the building pieces, the edit tool and the Creative phone,
    // which the island's read-only state takes and gives back on its own (Athena_PlayerController's HandlePermissions).
    // The image has no caller or binding of it left (a Final, non-Blueprint UFunction); its callers here are a Creative
    // game's reset of its players (AFortMinigame::ClearPlayerInventory) and the trip back to the hub
    // (PostLobbyIslandTeleport). Returns how many stacks went.
    DefineUFunction(ClearDroppableItems)
    int32_t ClearDroppableItems();
    // Core's (no Epic function by this name): what a Creative player's new pawn would not have carried over, where the
    // build's stripped respawns are replaced by moving the same pawn (a game's start and end, through
    // AFortMinigame::ClearPlayerInventory, and the trip back to the hub, PostLobbyIslandTeleport). The Movement
    // Modulator's GE_Toy_SpeedAdjust comes off the player state's ability system, and a flying player stops flying through
    // the game's own ServerCreativeStopFly and ClientCreativeStopFly (#512). Nothing where the build has neither.
    void ResetCreativeMovementState();
    // Athena_PlayerController's RevokeItem calls it right after it takes the Creative phone (K2_RemoveItemFromPlayer of
    // WID_CreativeTool): on leaving every island (OnLeaveVolume), on losing edit (HandlePermissions) and from
    // RevokeCreativeTool. Its exec is the shared empty one on 7.20 to 8.00, and no build has the body. Core's (#554): where
    // the quickbars are the client's (7.40), the held weapon whose item just went is handed to the game's own pending
    // destroy handler, as the pawn's ability-ended handler would on the next ability's end.
    DefineUFunction(MoveToolRemoved)
    void MoveToolRemoved();
    DECLARE_FUNCTION(execMoveToolRemoved);

    // 9.00 (CL-6337466): the Creative phone is AFortPlayerControllerGameplay's to give (#658). Athena_PlayerController's
    // HandlePermissions only calls SetIsCreativeModeEnabled, which broadcasts OnCreativeModeEnabledChangedDelegate; the
    // phone's load and grant (GrantCreativePhoneItemDefLoadedCallback, whose exec calls the shared empty function) were
    // compiled out. Before 9.00 the blueprint gives WID_CreativeTool itself.
    DefineUFunction(GrantCreativePhoneItemDefLoadedCallback)
    DefineUFunction(ClientEnsureCreativePhoneIsInProperSlot)
    DefineUFunction(OnCreativeModeEnabledChanged)
    DefineUFunction(IsCreativeModeEnabled)
    DefineUProperty(TAssetPtr<UFortWorldItemDefinition>, CreativeMoveToolItemDefinition)
    DefineUProperty(FMulticastScriptDelegate, OnCreativeModeEnabledChangedDelegate)
    void GrantCreativePhoneItemDefLoadedCallback();
    DECLARE_FUNCTION(execGrantCreativePhoneItemDefLoadedCallback);
    // Core's handler of OnCreativeModeEnabledChangedDelegate (bound at each Possess): the phone's load as the creative
    // mode comes on, its removal as it goes off.
    void HandleCreativeModeEnabledChanged(bool bIsEnabled);
    DECLARE_FUNCTION(execHandleCreativeModeEnabledChanged);
    static void BindCreativeModeEnabledChanged(AFortPlayerControllerAthena* Controller);

    void ClientCycleQuickBarToCreativeItem(const FGuid& CreativeItemGuid, UFortWorldItemDefinition* CreativeItemDef);
    // The game's own ServerLoadPlotForPortal_Implementation (7.00 0xC65E80): finds PlotItemId in the player's creative
    // profile, loads it into Portal's island (through the call Core restores) and makes it their last used plot. 7.20
    // strips it, and this is Core's restored one.
    void LoadPlotForPortal(AFortAthenaCreativePortal* Portal, const FString& PlotItemId);
    // Restored where the build strips both halves of the RPC (7.20: its exec, 0x175C530, calls the return-zero stub as
    // _Validate, slot 0x3FC, and the empty one as _Implementation, slot 0x3FD), as 7.10 has them (0xC84F20 and
    // 0xC84FC0, slots 0x3FF and 0x400).
    bool ServerLoadPlotForPortal_Validate(AFortAthenaCreativePortal* Portal, const FString& PlotItemId);
    void ServerLoadPlotForPortal_Implementation(AFortAthenaCreativePortal* Portal, const FString& PlotItemId);
    // The player's rift as they come into a Creative match (their first pawn), with their last used island loading
    // into it; given back as they leave.
    void ClaimCreativePortal();
    // Restored (7.00 strips it; 21.40 has it by this name): the player's islands from their creative profile into
    // CreativeIslands, which replicates to them (the rift's "Set Rift Destination" list and the island menu read it), and
    // their last used one (the profile's last_used_plot stat) into LastUsedCreativeIsland.
    void LoadCreativePlots();
    static void ReleaseCreativePortal(AFortPlayerState* PlayerState);
    // Whether the world's match is a Creative playlist's (bEnableCreativeMode, from 6.01).
    static bool IsCreativeMatch(const UObject* WorldContextObject);
    // From 7.00, where the build strips them: the Creative RPCs above, and the redirect of ServerLoadPlotForPortal's
    // stripped call.
    static void HookCreativeVTables();

    // 7.40's (CL-5046157) new Creative RPCs: no earlier build has them (7.30 neither). Their _Implementations are the
    // shared empty function (slots 0x430, 0x42E, 0x420); their _Validates are the game's (0xDA6F10 for the rift two: the
    // playlist's and UFortGlobals' bEnableCreativeMode; 0xDA72A0 for the auto restart: the same, or the game's Creative
    // check 0xDB87B0). Their only senders in the image are the cheat manager's ClaimPortal, ReleasePortal and
    // ToggleAutoRestartMinigame, stripped too (0x866D10, slot 0xA0). No build has the bodies: Core's, from 7.40's own pieces.
    DefineUFunction(ServerClaimPortal)
    DefineUFunction(ServerReleasePortal)
    DefineUFunction(ServerToggleAutoRestartMinigame)
    // The minigame of the island the player's state is in (7.40 0xD8F980: the volume manager's volume for the player, its
    // UFortMinigameVolumeComponent's Minigame), from 7.00.
    DefineUFunction(GetMinigame)
    // The player's rift of the hub, as they get one coming into the match (ClaimCreativePortal).
    void ServerClaimPortal_Implementation();
    // Their rift given back to the hub's portal manager, as when they leave (ReleaseCreativePortal).
    void ServerReleasePortal_Implementation();
    // The game of the island they are on, when they may run it, toggles its auto restart (AFortMinigame::
    // ToggleAutoRestartMinigame, the game's own).
    void ServerToggleAutoRestartMinigame_Implementation();
    class AFortMinigame* GetMinigame();

    // 9.00's (CL-6337466) Creative flight speed and sprint, AFortPlayerControllerGameplay's (Core models the controller
    // without that class). No earlier build has them. Their _Implementations are the shared empty function (slots 0x3A9
    // and 0x3A7); their _Validates are the game's (0x1801440, whether the player may fly). The client only sends them
    // (0x17E4CC0 cycles FlyingModifierIndex through FlyingModifiers; 0x17E49F0 toggles bIsFlightSprinting, 0x17F0260
    // ends it once the pawn slows down) and applies them in the OnReps: OnRep_FlyingModifierIndex (0x17EEFD0) clamps the
    // index, sets the athena movement component's speed (0x17FFC90: FlyingModifiers[Index], scaled while sprinting) and
    // broadcasts OnCreativeFlyingSpeedChanged; OnRep_IsFlightSprinting (0x17EF28A) sets the speed again.
    DefineUFunction(ServerCreativeSetFlightSpeedIndex)
    DefineUFunction(ServerCreativeSetFlightSprint)
    DefineUFunction(OnRep_FlyingModifierIndex)
    DefineUFunction(OnRep_IsFlightSprinting)
    DefineUProperty(int32_t, FlyingModifierIndex)
    DefineUProperty(bool, bIsFlightSprinting)
    void ServerCreativeSetFlightSpeedIndex_Implementation(int32_t Index);
    void ServerCreativeSetFlightSprint_Implementation(bool bSprint);

    // 9.00's Creative physics ball hit (#715). The ball's blueprint (CreativeBall_C) gives a player pawn's overlap to
    // ApplyPredictedImpulseFromPawnHitClient only on clients (9.21 0x11D2AE0): the client pushes its own ball and sends
    // the hit here. The server's own hit handler (0x11D75D0) only queues a player-controlled pawn's hit for this RPC.
    // Its _Implementation is the shared empty function (9.21 slot 0x41E; _Validate 0x1138570 is the game's).
    DefineUFunction(SendPhysicsBallHitToServer)
    void SendPhysicsBallHitToServer_Implementation(APhysicsObject* PhysicsObject, float ClientPing, APawn* HittingPawn, const FVector& VelocityOfHitter, const FVector& HitNormal,
        const FTimeStampedPhysicsPawnState& ClientBallState);

    // 7.10's (CL-4667333) island tools (PongooDev/CoreV2#488; FortPlayerControllerAthenaCreativeIslands.cpp): Publish
    // Island, Backup Island and Restore of the island menu, and an island code entered at a rift. Nothing on builds
    // without them (7.00 has none of these RPCs).
    DefineUProperty(bool, bIsAllowedToPublish)
    DefineUFunction(PublishMyCurrentCreativePlot)
    DefineUFunction(ClientPublishMyCurrentCreativePlotComplete)
    // 7.40 (CL-5046157): the publish names the island's volume, and its answer is ClientPublishCreativePlotComplete.
    DefineUFunction(PublishCreativePlot)
    DefineUFunction(ClientPublishCreativePlotComplete)
    DefineUFunction(UpdateCreativePlotData)
    DefineUFunction(ServerSaveIslandCheckpoint)
    DefineUFunction(ServerLoadIslandCheckpoint)
    DefineUFunction(ServerLoadPlotForPortalFromMnemonic)
    // Restored (7.10 strips its body: 0x3918A0 only destroys the by-value options): the island the player is on, when it
    // is theirs, is saved and published through the links service; the client hears ClientPublishMyCurrentCreativePlotComplete.
    void PublishMyCurrentCreativePlot_Implementation(const FCreativePublishOptions& MyPublishOptions);
    // Restored (7.40 strips its body: 0xDA3950 only destroys the by-value options): the island in VolumeToPublish, when
    // it is the player's and loaded, as PublishMyCurrentCreativePlot publishes the one they are on.
    void PublishCreativePlot_Implementation(AFortVolume* VolumeToPublish, const FCreativePublishOptions& MyPublishOptions);
    void ClientPublishMyCurrentCreativePlotComplete(const FString& ResultErrorCode, const FString& LinkJson);
    // Restored (7.40 strips its body: 0xDB4300 only destroys the by-value info): the island's new name and description
    // saved to its plot item (the creative profile's SetCreativePlotMetadata); the client hears
    // ClientPublishCreativePlotComplete, which its Description tab waits on.
    void UpdateCreativePlotData_Implementation(AFortVolume* VolumeToPublish, const FCreativeIslandInfo& MyInfo);
    void OnUpdateCreativePlotDataComplete(const FMcpQueryResult& Result);
    // Restored (7.10's are the shared empty function, slots 0x3FC and 0x3FA): the island's checkpoint, its backup.
    void ServerSaveIslandCheckpoint_Implementation(AFortAthenaCreativePortal* Portal);
    void ServerLoadIslandCheckpoint_Implementation(AFortAthenaCreativePortal* Portal);
    // As 7.10 has them (0xC84F20, the same check as ServerLoadPlotForPortal's, and 0xC84E10), where a build strips them.
    bool ServerLoadPlotForPortalFromMnemonic_Validate(AFortAthenaCreativePortal* Portal, const FString& Mnemonic);
    void ServerLoadPlotForPortalFromMnemonic_Implementation(AFortAthenaCreativePortal* Portal, const FString& Mnemonic);
    // Whether the player may publish (an enabled Support-A-Creator creator on Core-Backend): bIsAllowedToPublish, which
    // the island menu shows Publish Island for. Asked as the player comes into a Creative match.
    void QueryPublishingPermission();
    // Core's: the published island Volume holds put on the player's Creative island history (Core-Backend's
    // /core/api/creative/history), as they arrive on it. Nothing for an island without a code.
    void RecordCreativeIslandHistory(AFortVolume* Volume);
    static void HookCreativeIslandTools();
    // The featured rifts' codes from Core-Backend into UFortRuntimeOptions' CuratedLinkCodes, before the hub begins play.
    static void LoadCuratedLinkCodes();

private:
    static void PublishMyCurrentCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This, FCreativePublishOptions* MyPublishOptions);
    static inline void (*PublishMyCurrentCreativePlot_ImplementationOG)(AFortPlayerControllerAthena* This, FCreativePublishOptions* MyPublishOptions) = nullptr;
    static void PublishCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This, AFortVolume* VolumeToPublish, FCreativePublishOptions* MyPublishOptions);
    static inline void (*PublishCreativePlot_ImplementationOG)(AFortPlayerControllerAthena* This, AFortVolume* VolumeToPublish, FCreativePublishOptions* MyPublishOptions) = nullptr;
    static void UpdateCreativePlotData_ImplementationHK(AFortPlayerControllerAthena* This, AFortVolume* VolumeToPublish, FCreativeIslandInfo* MyInfo);
    static inline void (*UpdateCreativePlotData_ImplementationOG)(AFortPlayerControllerAthena* This, AFortVolume* VolumeToPublish, FCreativeIslandInfo* MyInfo) = nullptr;
    static void ServerSaveIslandCheckpoint_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal);
    static void ServerLoadIslandCheckpoint_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal);
    static bool ServerIslandCheckpoint_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal);
    static bool ServerLoadPlotForPortalFromMnemonic_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& Mnemonic);
    static void ServerLoadPlotForPortalFromMnemonic_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& Mnemonic);
    // The island the player is on, when it is theirs and loaded (its component), else null with Why set.
    class UFortLevelSaveComponent* GetOwnLoadedIsland(AFortAthenaCreativePortal* Portal, const wchar_t*& Why);
    // The same for the island in Volume.
    class UFortLevelSaveComponent* GetOwnLoadedIslandIn(AFortVolume* Volume, const wchar_t*& Why);

public:

    static void HookCallSites();
    // Restores the pawn-death handler (OnPawnDied) and the Athena part of PrepareDeathReport where the build strips them
    // (1.9.1), and ServerSuicide (7.40).
    static void HookVTables();
    // FortPlayerControllerAthenaDeath.cpp: the self-destruct (Suicide's RPC). 7.40 (CL-5046157) gives this class its own
    // _Implementation, the shared empty function; Core's runs the one 7.30 inherited.
    void ServerSuicide_Implementation();
    // -1 from 7.40 (CL-5046157), which drops the controller's TeamMembers and SquadMembers.
    static ptrdiff_t TeamMembersOffset() { return Member_TeamMembers.IsAvailable() ? Member_TeamMembers.GetOffsetChecked() : -1; }
    static bool HasTeamMembers() { return Member_TeamMembers.IsAvailable(); }
    static ptrdiff_t PlayerDeathReportOffset() { return Member_PlayerDeathReport.GetOffsetChecked(); }
    // -1 before 8.00 (CL-5203069), which adds the controller's MarkerComponent.
    static ptrdiff_t MarkerComponentOffset() { return Member_MarkerComponent.IsAvailable() ? Member_MarkerComponent.GetOffsetChecked() : -1; }

private:
    // _Implementation's parameters by value: the caller's copies, which the callee destroys (the original's body does).
    static void ServerPlaySquadQuickChatMessage_ImplementationHK(AFortPlayerControllerAthena* This, FAthenaQuickChatActiveEntry* ChatEntry, void* SenderID);
    static inline void (*ServerPlaySquadQuickChatMessage_ImplementationOG)(AFortPlayerControllerAthena* This, FAthenaQuickChatActiveEntry* ChatEntry, void* SenderID) = nullptr;
    static void ServerSendSquadFriend_ImplementationHK(AFortPlayerControllerAthena* This, AFortPlayerStateAthena* FriendInSquad);
    static void ServerEnableAnonymousMode_ImplementationHK(AFortPlayerControllerAthena* This);

    static void ServerGiveCreativeItem_ImplementationHK(AFortPlayerControllerAthena* This, const FFortItemEntry& CreativeItem);
    static void ServerReloadCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerStopSavingCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ResetMyCurrentCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerTeleportToPlaygroundIslandDock_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerTeleportToPlaygroundLobbyIsland_ImplementationHK(AFortPlayerControllerAthena* This);
    static bool ServerLoadPlotForPortal_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& PlotItemId);
    static void ServerLoadPlotForPortal_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& PlotItemId);
    static void ServerClaimPortal_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerReleasePortal_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerToggleAutoRestartMinigame_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerCreativeSetFlightSpeedIndex_ImplementationHK(AFortPlayerControllerAthena* This, int32_t Index);
    static void ServerCreativeSetFlightSprint_ImplementationHK(AFortPlayerControllerAthena* This, bool bSprint);
    static void SendPhysicsBallHitToServer_ImplementationHK(AFortPlayerControllerAthena* This, APhysicsObject* PhysicsObject, float ClientPing, APawn* HittingPawn,
        const FVector& VelocityOfHitter, const FVector& HitNormal, const FTimeStampedPhysicsPawnState& ClientBallState);
    // Whether Core restored ServerLoadPlotForPortal_Implementation (7.20), which LoadPlotForPortal then calls.
    static inline bool bServerLoadPlotForPortalRestored = false;
    // The player's island space: the one they stand in when it is theirs to edit, else their rift's.
    AFortVolume* GetCreativeVolume();
    // The creative profile's ItemsUpdatedDelegate (its query's answer, or a command that changed its plots): the islands
    // again, and the last used one into the player's rift if it has none yet.
    void OnCreativeProfileItemsUpdated(int64_t UpdateArgument);
    // The player's last used island (else their first) into their rift, unless the rift's island space has one loaded.
    void LoadLastUsedPlotIntoPortal();
    // The player's creative profile on the server (AFortPlayerController::CreativeModeProfile), null without one.
    UMcpProfile* GetCreativeProfile() const;
    bool TeleportPawnTo(const FVector& Location, const FRotator& Rotation, const wchar_t* Where);

    static void PawnDiedInventoryDropCall(AFortPlayerController* Controller, void* Unused, const FGameplayTagContainer* DamageTags, AFortPawn* DeadPawn, bool* bOutDroppedBackpack);

    // FortPlayerControllerAthenaDeath.cpp: 1.9's AFortPlayerControllerAthena::OnPawnDied (0x42DB20), which 1.9.1 strips.
    // DiedPawn: the pawn the handler is given from 9.00 (21.40's signature), null before.
    void OnPawnDied(AFortPlayerPawn* DiedPawn, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer);
    bool IsTeamOut();
    static void OnPawnDiedHK(AFortPlayerControllerAthena* This, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer);
    static void OnPawnDiedWithPawnHK(AFortPlayerControllerAthena* This, AFortPlayerPawn* DiedPawn, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer);
    // FortPlayerControllerAthenaDeath.cpp: the override of AFortPlayerControllerZone::PrepareDeathReport, whose Athena
    // game mode part 1.9.1 and 1.10 strip.
    void PrepareDeathReport(float LethalDamage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* KillerController, AActor* DamageCauser, bool bDroppedBackpack);
    static void PrepareDeathReportHK(AFortPlayerControllerAthena* This, float LethalDamage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* KillerController, AActor* DamageCauser, bool bDroppedBackpack);
    // FortPlayerControllerAthenaDeath.cpp: ServerSuicide_Implementation where this class's slot is a stub and its parent's
    // isn't (7.40).
    static void HookSuicideVTables();
    static void ServerSuicide_ImplementationHK(AFortPlayerControllerAthena* This);
    // The parent class's own _Implementation (7.40: AFortPlayerControllerZone's 0x1464AE0, which AFortPlayerControllerPvP
    // inherits).
    static inline void (*ServerSuicide_ImplementationSuper)(AFortPlayerControllerAthena* This) = nullptr;
};
