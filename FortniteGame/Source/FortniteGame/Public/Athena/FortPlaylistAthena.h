#pragma once

#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class UDataTable;

// A playlist's settings as a data asset (from 3.5, where it replaced AFortAthenaPlaylistBase and the game mode's
// PlaylistDataTables). AFortGameStateAthena::SetCurrentPlaylistId (3.5.1 0x9FA020) looks the match's up by its id and
// keeps it as CurrentPlaylistData; ApplyPlaylistDataTables (0x9DC890) loads its tables and copies them over the
// default ones.
class UFortPlaylist : public UObject
{
    DefineUnrealClass(UFortPlaylist, FortniteGame)

public:
    DefineUProperty(TAssetPtr<UDataTable>, LootTierData)
    DefineUProperty(TAssetPtr<UDataTable>, LootPackages)
    // The most players on a team. AFortGameModeAthena's FinishRestartPlayer (3.5.1 0x9E43D0) treats a large team game's
    // playlist with teams of 20 at most as Teams of 20, and one with bigger teams as 50v50.
    DefineUProperty(int32_t, MaxTeamSize)
    static bool HasMaxTeamSize() { return Member_MaxTeamSize.IsAvailable(); }
};

// Whether the players of a playlist come back after they die (from 3.5; 4.2's Playground: InfiniteRespawnExceptStorm).
class EAthenaRespawnType
{
    DefineUnrealEnum(EAthenaRespawnType, FortniteGame)
    DefineUEnumValue(None)
    DefineUEnumValue(InfiniteRespawn)
    DefineUEnumValue(InfiniteRespawnExceptStorm)
};

class UFortPlaylistAthena : public UFortPlaylist
{
    DefineUnrealClass(UFortPlaylistAthena, FortniteGame)

public:
    DefineUProperty(int32_t, PlaylistId)
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
};
