#pragma once

class UFortGameData;

// Cert CL-3532353's LootTierData_Client, LootPackages_Client and LootQuotaData. 1.2's game data still names the first
// two but its pak ships neither, and its LootQuotaData is null; the tables are created in memory only then.
namespace FortLootTablesCert
{
    void CreateMissingTables(UFortGameData* GameData);
}
