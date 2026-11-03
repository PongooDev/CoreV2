#pragma once

#include "GameFramework/Info.h"

class UDataTable;

// A Battle Royale playlist's settings (3.0 to Season 4; UFortPlaylistAthena, a data asset, replaced it later). Each
// playlist is a Blueprint class of it; AFortGameModeAthena::PlaylistDataTables names them, and ApplyPlaylistDataTables
// (3.0 0x99AB50) reads the class default object whose PlaylistId is the match's.
class AFortAthenaPlaylistBase : public AInfo
{
    DefineUnrealClass(AFortAthenaPlaylistBase, FortniteGame)

public:
    DefineUProperty(int32_t, PlaylistId)
    DefineUProperty(UDataTable*, LootTierData)
    DefineUProperty(UDataTable*, LootPackages)
};
