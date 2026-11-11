#pragma once

#include "GameplayEffect.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class UDataTable;
class UWorld;

// A playlist's settings as a data asset (from 3.5, where it replaced AFortAthenaPlaylistBase and the game mode's
// PlaylistDataTables). AFortGameStateAthena::SetCurrentPlaylistId (3.5.1 0x9FA020) looks the match's up by its id and
// keeps it as CurrentPlaylistData; ApplyPlaylistDataTables (0x9DC890) loads its tables and copies them over the
// default ones.
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
    // The most players on a team. AFortGameModeAthena's FinishRestartPlayer (3.5.1 0x9E43D0) treats a large team game's
    // playlist with teams of 20 at most as Teams of 20, and one with bigger teams as 50v50.
    DefineUProperty(int32_t, MaxTeamSize)
    static bool HasMaxTeamSize() { return Member_MaxTeamSize.IsAvailable(); }
    // The most players in the match: 100 on every Battle Royale playlist of these seasons, 4 on Playground (5.10's
    // Playlist_Playground: 4 teams of up to 4, MaxPlayers 4). Battle Royale's player bots fill no further (CoreV2 #374).
    DefineUProperty(int32_t, MaxPlayers)
    static bool HasMaxPlayers() { return Member_MaxPlayers.IsAvailable(); }
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
    DefineUProperty(bool, bAllowTeamSwitching)
    static bool HasAllowTeamSwitching() { return Member_bAllowTeamSwitching.IsAvailable(); }
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
