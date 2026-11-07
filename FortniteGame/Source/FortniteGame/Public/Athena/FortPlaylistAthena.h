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
};
