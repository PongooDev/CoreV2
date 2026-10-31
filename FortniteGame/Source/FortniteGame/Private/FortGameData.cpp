#include "pch.h"
#include "FortGameData.h"
#include "Engine/DataTable.h"
#include "Items/FortLootPackage.h"
#include "Items/FortLootTablesCert.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortLoot, Log, All);

namespace
{
    std::vector<UDataTable*> LoadTables(TArray<TAssetPtr<UDataTable>>& Tables)
    {
        std::vector<UDataTable*> Loaded;
        for (int32_t Index = 0; Index < Tables.Num(); ++Index)
        {
            if (UDataTable* DataTable = SoftElementAt(Tables, Index).LoadSynchronous())
            {
                Loaded.push_back(DataTable);
            }
        }
        return Loaded;
    }
}

std::vector<UDataTable*> UFortGameData::GetLootTierDataTables()
{
    FortLootTablesCert::CreateMissingTables(this);
    return LoadTables(LootTierDataTables);
}

std::vector<UDataTable*> UFortGameData::GetLootPackageDataTables()
{
    FortLootTablesCert::CreateMissingTables(this);
    return LoadTables(LootPackageDataTables);
}

bool UFortGameData::HasLootTierDataTables()
{
    return !GetLootTierDataTables().empty();
}

bool UFortGameData::HasLootPackageDataTables()
{
    return !GetLootPackageDataTables().empty();
}

UDataTable* UFortGameData::GetLootQuotaData()
{
    FortLootTablesCert::CreateMissingTables(this);
    return LootQuotaData;
}

FFortLootTierData* UFortGameData::FindLootTierDataRow(FName LootTierKey, const wchar_t* ContextString)
{
    for (UDataTable* Table : GetLootTierDataTables())
    {
        if (FFortLootTierData* Row = Table->FindRow<FFortLootTierData>(LootTierKey, ContextString, false))
        {
            return Row;
        }
    }

    UE_LOG(LogFortLoot, Warning, TEXT("UFortGameData::FindLootTierDataRow : TierGroupName %s with context %s not found!"), *LootTierKey.ToString(), ContextString);
    return nullptr;
}
