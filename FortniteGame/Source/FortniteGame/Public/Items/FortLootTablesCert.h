#pragma once

class UFortGameData;

// Cert CL-3532353's LootTierData_Client, LootPackages_Client and LootQuotaData, for builds whose pak ships none of them
// (1.2: its game data still names the first two, and LootQuotaData is null). Only then are they created in memory, at
// the paths the game data names; on Cert the real assets load and these rows are never read.
namespace FortLootTablesCert
{
    void CreateMissingTables(UFortGameData* GameData);
}
