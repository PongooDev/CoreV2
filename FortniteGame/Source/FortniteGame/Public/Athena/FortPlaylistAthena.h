#pragma once

#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Engine/NetSerialization.h"
#include "GameplayEffect.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class UDataTable;
class UPlaylistUserOptions;
class UWorld;

// A playlist's settings as a data asset (from 3.5, where it replaced AFortAthenaPlaylistBase and the game mode's
// PlaylistDataTables). AFortGameStateAthena::SetCurrentPlaylistId (3.5.1 0x9FA020) looks the match's up by its id and
// keeps it as CurrentPlaylistData (from 6.01 in CurrentPlaylistInfo, an FPlaylistPropertyArray); ApplyPlaylistDataTables
// (0x9DC890) loads its tables and copies them over the default ones. From 5.21 it copies the loot tables no more: the
// game mode's GetLootTierDataTables and GetLootPackageDataTables give the playlist's own (AFortGameMode).
class UFortPlaylist : public UObject
{
    DefineUnrealClass(UFortPlaylist, FortniteGame)

public:
    // The name the playlist manager finds it by (Playlist_DefaultSolo), the one the matchmaking service's assignment
    // gives from 5.30 (HandleAssignMatchMessage's "PlaylistName: %s").
    DefineUProperty(FName, PlaylistName)
    // The levels the match streams in with the playlist (AFortGameState::StartStreamingAdditionalPlaylistLevel).
    DefineUProperty(TArray<TAssetPtr<UWorld>>, AdditionalLevels)
    static bool HasAdditionalLevels() { return Member_AdditionalLevels.IsAvailable(); }
    DefineUProperty(TAssetPtr<UDataTable>, LootTierData)
    DefineUProperty(TAssetPtr<UDataTable>, LootPackages)
    static bool HasLootTables() { return Member_LootTierData.IsAvailable() && Member_LootPackages.IsAvailable(); }
    static ptrdiff_t LootTierDataOffset() { return Member_LootTierData.GetOffsetChecked(); }
    static ptrdiff_t LootPackagesOffset() { return Member_LootPackages.GetOffsetChecked(); }
    // The most players on a team. AFortGameModeAthena's FinishRestartPlayer (3.5.1 0x9E43D0) treats a large team game's
    // playlist with teams of 20 at most as Teams of 20, and one with bigger teams as 50v50.
    DefineUProperty(int32_t, MaxTeamSize)
    static bool HasMaxTeamSize() { return Member_MaxTeamSize.IsAvailable(); }
    // The most players in the match: 100 on every Battle Royale playlist of these seasons, 4 on Playground (5.10's
    // Playlist_Playground: 4 teams of up to 4, MaxPlayers 4). Battle Royale's player bots fill no further (CoreV2 #374).
    DefineUProperty(int32_t, MaxPlayers)
    static bool HasMaxPlayers() { return Member_MaxPlayers.IsAvailable(); }
    // The options the playlist's players set in the lobby (from 6.01; Playlist_Playground's Playgrounds_UserKnobsV1),
    // none on most playlists.
    DefineUProperty(UPlaylistUserOptions*, UserOptions)
    static bool HasUserOptions() { return Member_UserOptions.IsAvailable(); }

    // A Showdown playlist, one of 6.10's in-game tournaments' (CL-4480234; Playlist_Showdown_Solo, _Duos and _Squads,
    // 58071 to 58073): the playlist name is one the tournament manager gives a match type, as its own lookup has it
    // (UFortTournamentManager, CL-4480234 0x11A7750: the three names, compared without their case, give
    // EFortShowdownMatchType Solo, Duos and Squads, any other Unknown). 6.20's (CL-4497486 0x11E9D90) also names
    // Fortnitemares' tournament playlists, Playlist_Deimos_SoloShow, _DuoShow and _SquadShow (59071 to 59073, bIsTournament,
    // with the same ShowdownScoringHUD UI extension). Their matches are the tournaments' (the client adds the scoring HUD
    // alert as the playlist's UI extension, and Core-Backend scores their matches for the event they play in), and their
    // server tells their players the team's eliminations and when its stats are final
    // (AFortPlayerStateAthena::ClientReportTeamKill and ClientNotifyTournamentStatsFinal). The name is Core's; no build
    // before 6.10 has the three playlists, none before 6.20 the Fortnitemares ones.
    bool IsShowdownPlaylist() const;
};

// Whether the players of a playlist come back after they die (from 3.5; 4.2's Playground: InfiniteRespawnExceptStorm).
class EAthenaRespawnType
{
    DefineUnrealEnum(EAthenaRespawnType, FortniteGame)
    DefineUEnumValue(None)
    DefineUEnumValue(InfiniteRespawn)
    DefineUEnumValue(InfiniteRespawnExceptStorm)
};

// How a playlist's match is won (from 3.6). FirstToGoalScore is Score Royale's (5.30's Playlist_Score_Solo, Duos and
// Squads): the first team whose score reaches its ScoringData's GoalScore wins.
class EAthenaWinCondition
{
    DefineUnrealEnum(EAthenaWinCondition, FortniteGame)
    DefineUEnumValue(LastManStanding)
    DefineUEnumValue(FirstToGoalScore)
};

// A FirstToGoalScore playlist's scoring (from 4.4): the score that wins and the points of each scoring event, whose
// ScoreAwarded rows are the playlist's own AthenaGameData (Default.ScoringEvent.*).
struct FWinConditionScoreData
{
    DefineUnrealStruct(FWinConditionScoreData, FortniteGame)
    DefineUStructProperty(FScalableFloat, GoalScore)
};

class UFortPlaylistAthena : public UFortPlaylist
{
    DefineUnrealClass(UFortPlaylistAthena, FortniteGame)

public:
    DefineUProperty(int32_t, PlaylistId)
    // EAirCraftBehavior (1: OpposingAirCraftForEachTeam), which the game state takes with the playlist.
    DefineUProperty(uint8_t, AirCraftBehavior)
    // The AI of the playlist's match (UAthenaAISettings), which the game mode takes as its AISettings; Battle Royale's
    // own playlists have none.
    DefineUProperty(UObject*, AISettings)
    static bool HasAISettings() { return Member_AISettings.IsAvailable(); }
    // ESafeZoneStartUp, when the storm circle comes: 0 UseDefaultGameBehavior (after the bus), 1
    // StartsWithWarmUp, 2 StartsWithAirCraft (50v50's, whose final circle shows on the map from the bus on).
    DefineUProperty(uint8_t, SafeZoneStartUp)
    static bool HasSafeZoneStartUp() { return Member_SafeZoneStartUp.IsAvailable(); }
    // The game's own code plays a FirstToGoalScore playlist (5.30): AFortPlayerStateAthena scores the playlist's events
    // into TotalPlayerScore and its AFortTeamInfoAthena's score (a searched container's tags in
    // AFortPlayerController::ServerAttemptInteract, a supply drop's or llama's in AFortAthenaSupplyDrop::
    // SpawningLootOnDestruction, a coin's in AFortSpawnedScoreActor::OnScoreActorCollected, the eliminations in
    // OnRep_Kills), and AFortGameModeAthena's Tick sorts the teams by score and ends the match for the first at GoalScore
    // (5.30 0xC006E0 and 0xBD9E80: the team places, ClientNotifyWon, StartEndGamePhase's end).
    DefineUProperty(EAthenaWinCondition, WinConditionType)
    DefineUStructProperty(FWinConditionScoreData, ScoringData)
    static bool HasScoringData() { return Member_WinConditionType.IsAvailable() && Member_ScoringData.IsAvailable(); }
    bool IsScoringPlaylist() const { return HasScoringData() && WinConditionType == EAthenaWinCondition::GetFirstToGoalScore(); }
    // AFortGameModeAthena::OnPlaylistDataLoaded (4.2 0xA1BE40) allows respawning (an unreflected game mode flag) when it
    // isn't None; IsRespawningAllowed (0xA19780) reads it again for InfiniteRespawnExceptStorm.
    DefineUProperty(EAthenaRespawnType, RespawnType)
    static bool HasRespawnType() { return Member_RespawnType.IsAvailable(); }
    // How far over the death a respawning player comes back (from 5.41, and still the playlist's own on 6.01 and 6.02).
    // 6.00's Playlist_Playground has its game data's row Playground.RespawnHeight, 15000. Only the server read it: the
    // client builds keep no reader (6.00's AFortGameModeAthena::SpawnDefaultPawnFor_Implementation, 0xC35C70, is the
    // stripped stub, and nothing else in the game mode's or the controller's code takes the member's address).
    DefineUStructProperty(FScalableFloat, RespawnHeight)
    static bool HasRespawnHeight() { return Member_RespawnHeight.IsAvailable(); }
    // Set on Playground (4.5's Playlist_Playground, unlike Solo, Duo, Squad and 50v50): the match makes no progress on
    // the profile's quests, the battle pass and daily challenges. The build reads it only for the playlist's context
    // tags (4.5 AFortGameStateAthena::GetAthenaPlaylistContextTags, 0xA6A670); the quest progress the server sent from
    // it was Epic's to refuse, so Core's server sends none (CoreV2 #344).
    DefineUProperty(bool, bIgnoreDefaultQuests)
    static bool HasIgnoreDefaultQuests() { return Member_bIgnoreDefaultQuests.IsAvailable(); }
    // How many times a container's search and a floor loot spot pick their loot (from 5.0; 1 by default). 5.10's
    // Playground (CoreV2 #357) has 3: "Chests now spawn 3 weapons and more ammo & consumables than normal" and "Floor
    // loot now spawns 3 items"; 5.21's is back to 1 ("Chest & floor loot changed back to one weapon spawn each").
    // The search that read it, ABuildingContainer::SpawnLoot, is stripped from 5.10's and 5.21's client builds, whose
    // servers run Core's (Building/BuildingContainer.cpp), and nothing else in them reads it.
    DefineUProperty(int32_t, LootDropRounds)
    static bool HasLootDropRounds() { return Member_LootDropRounds.IsAvailable(); }
    // Set on Playground: its players change teams with the pause menu's team select (TeamSelectWidget, which calls
    // AFortPlayerControllerAthena::SetTeam, a server RPC; 5.10's SetTeam_Implementation 0xC0DD10 checks the team is in
    // the playlist's DefaultFirstTeam..DefaultLastTeam and calls the game mode's ChangeTeam, slot 0xA80). Read by
    // AFortGameModeAthena's team switches, at the switched pawn's death and its player's next Possess (CoreV2 #374).
    // A bool of its own up to 6.00; 6.01 (CL-4424678) packs it into a byte of flags with bEnableCreativeMode,
    // bEnablePlayspaceManager and bAllowLayoutRequirementsFeature (bit 2 of +0x4A8), so it is read by its mask.
    DefineBitfieldUProperty(bAllowTeamSwitching)
    static bool HasAllowTeamSwitching() { return Member_bAllowTeamSwitching.IsAvailable(); }
};

// One of a match's changes to its playlist's properties (from 6.01), an item of FPlaylistPropertyArray: the property
// PropertyName takes the text PropertyData. With no PropertyScope it is a property of the playlist itself, which every
// machine changes on its own copy of the playlist (OverridePlaylist; a client as the item replicates, CL-4424678
// 0xEDB300). With one it is a property of the objects whose class is in the package of that short name
// (CreativeMutator_Gravity), which take it when they ask the array (the game state's mutators as they spawn).
struct FPropertyOverride : public FFastArraySerializerItem
{
    DefineUnrealStruct(FPropertyOverride, FortniteGame)
    DefineUProperty(FString, PropertyScope)
    DefineUProperty(FString, PropertyName)
    DefineUProperty(FString, PropertyData)

public:
    static bool IsAvailable() { return StaticStruct() && Member_PropertyScope.IsAvailable() && Member_PropertyName.IsAvailable() && Member_PropertyData.IsAvailable(); }
};

// From 6.01 (CL-4424678): the match's playlist as the game state has and replicates it, AFortGameStateAthena's
// CurrentPlaylistInfo, which took the place of the CurrentPlaylistData pointer. 21.40's layout, which 6.01's matches:
// the fast array of PropertyOverrides, PlaylistReplicationKey, BasePlaylist, OverridePlaylist, PropertyOverrideScopes.
// Its NetDeltaSerialize (6.01 0xEF6450) writes a bit, then the key and BasePlaylist when the key isn't the one the
// connection's last state has (21.40's FNetPlaylistPropertyArrayState: ArrayState and PlaylistReplicationKey; a new
// connection has none), then the overrides' own fast array delta (0xEDB300). A client reads BasePlaylist from that,
// builds OverridePlaylist from the overrides it received and runs OnRep_CurrentPlaylistInfo.
struct FPlaylistPropertyArray : public FFastArraySerializer
{
    DefineUnrealStruct(FPlaylistPropertyArray, FortniteGame)
    DefineUStructArrayProperty(FPropertyOverride, PropertyOverrides)
    DefineUProperty(UFortPlaylistAthena*, BasePlaylist)
    DefineUProperty(UFortPlaylistAthena*, OverridePlaylist)
    // 6.01 doesn't reflect it: the int32 between PropertyOverrides and BasePlaylist (+0xC0; the game state's
    // constructor, 0xBF8331, starts it at -1).
    DefineUProperty(int32_t, PlaylistReplicationKey)

public:
    static bool IsAvailable() { return Member_BasePlaylist.IsAvailable() && Member_OverridePlaylist.IsAvailable(); }
    // -1 when the build's layout has no room for the key where 6.01 and 21.40 have it.
    static ptrdiff_t PlaylistReplicationKeyOffset()
    {
        if (Member_PlaylistReplicationKey.IsAvailable())
        {
            return Member_PlaylistReplicationKey.GetOffsetChecked();
        }
        if (!Member_PropertyOverrides.IsAvailable() || !Member_BasePlaylist.IsAvailable())
        {
            return -1;
        }
        const ptrdiff_t AfterOverrides = Member_PropertyOverrides.GetOffsetChecked() + static_cast<ptrdiff_t>(sizeof(FScriptArray));
        return Member_BasePlaylist.GetOffsetChecked() - AfterOverrides == static_cast<ptrdiff_t>(sizeof(void*)) ? AfterOverrides : -1;
    }

    // The playlist the match plays: the copy its overrides made when there is one, else the playlist itself. The game
    // has no function for it, every reader inlines the two loads (6.01's OnRep_CurrentPlaylistInfo, 0xC1DFF9:
    // "mov rcx, [rcx+18F0h]; test rcx, rcx; jnz; mov rcx, [rbx+18E8h]"; LoadCurrentPlaylistData, 0xC185D0, the same).
    UFortPlaylistAthena* GetCurrentPlaylist() const
    {
        UFortPlaylistAthena* Override = OverridePlaylist;
        return Override ? Override : static_cast<UFortPlaylistAthena*>(BasePlaylist);
    }

    // 21.40's name, on the server: the match's overrides become the ones its players chose for Playlist's UserOptions.
    // MatchOptions is the game state's (AFortGameState::GetMatchOptions), where AFortGameMode::OnGivenMatchAssignment
    // left each of the party's custom match options under its option's scoped key ("<scope>:<OptionKey>"); each of the
    // playlist's options found there becomes an item (PropertyScope the scope, PropertyName the OptionKey, PropertyData
    // the value's text), marked dirty so it replicates. An option of the playlist's own properties (no scope) is left
    // out. Returns how many it set.
    //
    // The build keeps both ends of this and strips the middle. The options reach the game state (CL-4424678 0xE481D0,
    // 0xE4C2A0, 0xE9A910), and AFortGameStateAthena's mutator observer (0xC19B80, on the authority) has this array give
    // each mutator that spawns the items of its scope (0xEE38F0: the items whose PropertyScope is the short name of the
    // mutator's class's package, each imported as text into the property it names, 0xEF3AA0), after which the game's
    // own code reads the mutators. But nothing left in the build reads the game state's MatchOptions or adds an item:
    // the functions that logged "PLAYLIST: Applied property override %s - %s" are gone.
    // 🟡 Inferred: that the stripped step takes the playlist's own options only (not any "<scope>:<name>" pair the
    // match options have), and that it runs when the playlist becomes the game state's. Both ends fix what an item
    // must hold; which pairs Epic's server accepted, and where it checked their values, can't be read from the build.
    int32_t SetPropertyOverrides(const UFortPlaylistAthena* Playlist, const TMap<FString, FString>* MatchOptions);
};

// The game mode's playlists (AFortGameMode's PlaylistManager): every Battle Royale playlist the build has.
class UFortPlaylistManager : public UObject
{
    DefineUnrealClass(UFortPlaylistManager, FortniteGame)

public:
    DefineUProperty(TArray<UFortPlaylistAthena*>, AthenaPlaylists)

    // 5.30 0xEEE5A0: the playlist named PlaylistName (null, with "UFortPlaylistManager::GetPlaylist: Unable to find
    // playlist %s", when there is none); 0xEEAE20: the one with the id.
    UFortPlaylistAthena* GetPlaylist(FName PlaylistName)
    {
        for (UFortPlaylistAthena* Playlist : static_cast<TArray<UFortPlaylistAthena*>&>(AthenaPlaylists))
        {
            if (Playlist && static_cast<FName>(Playlist->PlaylistName) == PlaylistName)
            {
                return Playlist;
            }
        }
        return nullptr;
    }
    UFortPlaylistAthena* GetPlaylist(int32_t PlaylistId)
    {
        for (UFortPlaylistAthena* Playlist : static_cast<TArray<UFortPlaylistAthena*>&>(AthenaPlaylists))
        {
            if (Playlist && Playlist->PlaylistId == PlaylistId)
            {
                return Playlist;
            }
        }
        return nullptr;
    }
};
