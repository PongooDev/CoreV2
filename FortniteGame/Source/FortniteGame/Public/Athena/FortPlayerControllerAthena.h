#pragma once

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

// One of a player's Creative islands as the controller replicates it (from 7.00): its name and its plot item's id.
struct FCreativeIslandData
{
    DefineUnrealStruct(FCreativeIslandData, FortniteGame)

public:
    DefineUProperty(FText, IslandName)
    DefineUProperty(FString, McpId)
};

// What a player publishes their island with (7.10's island menu, Publish Island: UFortCreativePublishModal):
// FCreativePublishOptions (7.10 SDK, 0x30 bytes), which PublishMyCurrentCreativePlot takes by value.
struct FCreativePublishOptions
{
    FString UserTitle;
    FString UserDescription;
    FString UserLocale;
};
static_assert(sizeof(FCreativePublishOptions) == 0x30);

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

    static bool HasGlider() { return Member_Glider.IsAvailable(); }
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
    DefineUFunction(SpectateOnDeath)
    // The build's SpectateOnDeath (1.9 0x438550, 3.3 0xA31650, 4.5 0xAAC400, 5.40 0xC34710), through its UFunction: what
    // the death's spectate timer calls (OnPawnDied).
    void SpectateOnDeath();
    // Server: a player who is back in play (a respawn, or a warmup restart) stops spectating after their death: the
    // death's spectate timer goes and the player state stops watching anyone (Core's: the game's RespawnPlayerAfterDeath,
    // 4.5 0xAA8410, leaves both alone, and SpectateOnDeath only returns early for a view target that is alive).
    void StopSpectatingOnDeath();
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

    // Creative (from 7.00; FortPlayerControllerAthenaCreative.cpp). The player's islands, replicated from their creative
    // profile, the last one they used, their rift of the hub and the island space it is linked to.
    DefineUStructArrayProperty(FCreativeIslandData, CreativeIslands)
    DefineUProperty(FString, LastUsedCreativeIsland)
    DefineUProperty(AFortVolume*, CreativePlotLinkedVolume)
    DefineUProperty(AFortAthenaCreativePortal*, OwnedPortal)
    static bool HasOwnedPortal() { return Member_OwnedPortal.IsAvailable() && Member_CreativePlotLinkedVolume.IsAvailable(); }
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

    // 7.10's (CL-4667333) island tools (PongooDev/CoreV2#488; FortPlayerControllerAthenaCreativeIslands.cpp): Publish
    // Island, Backup Island and Restore of the island menu, and an island code entered at a rift. Nothing on builds
    // without them (7.00 has none of these RPCs).
    DefineUProperty(bool, bIsAllowedToPublish)
    DefineUFunction(PublishMyCurrentCreativePlot)
    DefineUFunction(ClientPublishMyCurrentCreativePlotComplete)
    DefineUFunction(ServerSaveIslandCheckpoint)
    DefineUFunction(ServerLoadIslandCheckpoint)
    DefineUFunction(ServerLoadPlotForPortalFromMnemonic)
    // Restored (7.10 strips its body: 0x3918A0 only destroys the by-value options): the island the player is on, when it
    // is theirs, is saved and published through the links service; the client hears ClientPublishMyCurrentCreativePlotComplete.
    void PublishMyCurrentCreativePlot_Implementation(const FCreativePublishOptions& MyPublishOptions);
    void ClientPublishMyCurrentCreativePlotComplete(const FString& ResultErrorCode, const FString& LinkJson);
    // Restored (7.10's are the shared empty function, slots 0x3FC and 0x3FA): the island's checkpoint, its backup.
    void ServerSaveIslandCheckpoint_Implementation(AFortAthenaCreativePortal* Portal);
    void ServerLoadIslandCheckpoint_Implementation(AFortAthenaCreativePortal* Portal);
    // As 7.10 has them (0xC84F20, the same check as ServerLoadPlotForPortal's, and 0xC84E10), where a build strips them.
    bool ServerLoadPlotForPortalFromMnemonic_Validate(AFortAthenaCreativePortal* Portal, const FString& Mnemonic);
    void ServerLoadPlotForPortalFromMnemonic_Implementation(AFortAthenaCreativePortal* Portal, const FString& Mnemonic);
    // Whether the player may publish (an enabled Support-A-Creator creator on Core-Backend): bIsAllowedToPublish, which
    // the island menu shows Publish Island for. Asked as the player comes into a Creative match.
    void QueryPublishingPermission();
    static void HookCreativeIslandTools();
    // The featured rifts' codes from Core-Backend into UFortRuntimeOptions' CuratedLinkCodes, before the hub begins play.
    static void LoadCuratedLinkCodes();

private:
    static void PublishMyCurrentCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This, FCreativePublishOptions* MyPublishOptions);
    static inline void (*PublishMyCurrentCreativePlot_ImplementationOG)(AFortPlayerControllerAthena* This, FCreativePublishOptions* MyPublishOptions) = nullptr;
    static void ServerSaveIslandCheckpoint_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal);
    static void ServerLoadIslandCheckpoint_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal);
    static bool ServerIslandCheckpoint_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal);
    static bool ServerLoadPlotForPortalFromMnemonic_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& Mnemonic);
    static void ServerLoadPlotForPortalFromMnemonic_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& Mnemonic);
    // The island the player is on, when it is theirs and loaded (its component), else null with Why set.
    class UFortLevelSaveComponent* GetOwnLoadedIsland(AFortAthenaCreativePortal* Portal, const wchar_t*& Why);

public:

    static void HookCallSites();
    // Restores the pawn-death handler (OnPawnDied) and the Athena part of PrepareDeathReport where the build strips them
    // (1.9.1).
    static void HookVTables();
    // -1 from 7.40 (CL-5046157), which drops the controller's TeamMembers and SquadMembers.
    static ptrdiff_t TeamMembersOffset() { return Member_TeamMembers.IsAvailable() ? Member_TeamMembers.GetOffsetChecked() : -1; }
    static bool HasTeamMembers() { return Member_TeamMembers.IsAvailable(); }
    static ptrdiff_t PlayerDeathReportOffset() { return Member_PlayerDeathReport.GetOffsetChecked(); }

private:
    // _Implementation's parameters by value: the caller's copies, which the callee destroys (the original's body does).
    static void ServerPlaySquadQuickChatMessage_ImplementationHK(AFortPlayerControllerAthena* This, FAthenaQuickChatActiveEntry* ChatEntry, void* SenderID);
    static inline void (*ServerPlaySquadQuickChatMessage_ImplementationOG)(AFortPlayerControllerAthena* This, FAthenaQuickChatActiveEntry* ChatEntry, void* SenderID) = nullptr;

    static void ServerGiveCreativeItem_ImplementationHK(AFortPlayerControllerAthena* This, const FFortItemEntry& CreativeItem);
    static void ServerReloadCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerStopSavingCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ResetMyCurrentCreativePlot_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerTeleportToPlaygroundIslandDock_ImplementationHK(AFortPlayerControllerAthena* This);
    static void ServerTeleportToPlaygroundLobbyIsland_ImplementationHK(AFortPlayerControllerAthena* This);
    static bool ServerLoadPlotForPortal_ValidateHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& PlotItemId);
    static void ServerLoadPlotForPortal_ImplementationHK(AFortPlayerControllerAthena* This, AFortAthenaCreativePortal* Portal, const FString& PlotItemId);
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
    void OnPawnDied(float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer);
    bool IsTeamOut();
    static void OnPawnDiedHK(AFortPlayerControllerAthena* This, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer);
    // FortPlayerControllerAthenaDeath.cpp: the override of AFortPlayerControllerZone::PrepareDeathReport, whose Athena
    // game mode part 1.9.1 and 1.10 strip.
    void PrepareDeathReport(float LethalDamage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* KillerController, AActor* DamageCauser, bool bDroppedBackpack);
    static void PrepareDeathReportHK(AFortPlayerControllerAthena* This, float LethalDamage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* KillerController, AActor* DamageCauser, bool bDroppedBackpack);
};
