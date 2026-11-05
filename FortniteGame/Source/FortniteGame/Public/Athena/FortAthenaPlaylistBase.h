#pragma once

#include "GameFramework/Info.h"

class UDataTable;

// A Battle Royale playlist's settings (3.0 to 3.3; from 3.5 UFortPlaylistAthena, a data asset, replaced it). Each
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
