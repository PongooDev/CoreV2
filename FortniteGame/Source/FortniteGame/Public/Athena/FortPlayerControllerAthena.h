#pragma once

#include "Containers/Array.h"
#include "Items/FortItemDefinition.h"
#include "Math/Vector.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerControllerZone.h"
#include "UObject/WeakObjectPtr.h"

#include <vector>

class AActor;
class AController;
class APawn;
class AFortPawn;
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
    // The build's SpectateOnDeath (1.9 0x438550, 3.3 0xA31650, 5.40 0xC34710), through its UFunction: what the death's
    // spectate timer calls (OnPawnDied).
    void SpectateOnDeath();
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

    static void HookCallSites();
    // Restores the pawn-death handler (OnPawnDied) and the Athena part of PrepareDeathReport where the build strips them
    // (1.9.1).
    static void HookVTables();
    static ptrdiff_t TeamMembersOffset() { return Member_TeamMembers.GetOffsetChecked(); }
    static ptrdiff_t PlayerDeathReportOffset() { return Member_PlayerDeathReport.GetOffsetChecked(); }

private:
    // _Implementation's parameters by value: the caller's copies, which the callee destroys (the original's body does).
    static void ServerPlaySquadQuickChatMessage_ImplementationHK(AFortPlayerControllerAthena* This, FAthenaQuickChatActiveEntry* ChatEntry, void* SenderID);
    static inline void (*ServerPlaySquadQuickChatMessage_ImplementationOG)(AFortPlayerControllerAthena* This, FAthenaQuickChatActiveEntry* ChatEntry, void* SenderID) = nullptr;

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
