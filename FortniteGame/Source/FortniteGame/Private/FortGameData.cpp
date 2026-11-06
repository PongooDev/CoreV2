#include "pch.h"
#include "FortGameData.h"
#include "Engine/DataTable.h"
#include "Items/FortLootPackage.h"
#include "Items/FortLootTablesCert.h"
#include "Logging/LogMacros.h"
#include "UObject/UObjectGlobals.h"

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
                // The game data names its tables by soft references only, so nothing kept a loaded table: the next
                // garbage collection purged it and the next roll loaded it again from the pak. The rows a playlist's
                // tables put in AthenaLootTierData_Client and AthenaLootPackages_Client (AFortGameModeAthena::
                // ApplyPlaylistDataTables) went with it, and an LTM's chests gave the default loot from then on (#218).
                if (!DataTable->IsRooted())
                {
                    DataTable->AddToRoot();
                }
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
    // The PC cook empties LootQuotaData's reference on every build from 1.10 on, so a server-assets pak that carries the
    // server's table (Core-ServerAssets' list) is only found by its path. Tried once, after the Cert fallback, which
    // stands down when the game data already has a quota table.
    static bool bTriedServerAssets = false;
    if (!LootQuotaData && !bTriedServerAssets)
    {
        bTriedServerAssets = true;
        if (UDataTable* Table = LoadObject<UDataTable>(nullptr, L"/Game/Items/Datatables/LootQuotaData.LootQuotaData"))
        {
            Table->AddToRoot();
            LootQuotaData = Table;
            UE_LOG(LogFortLoot, Log, TEXT("Loot quotas: the server's LootQuotaData from the installed server assets (%d rows)"), Table->GetRowMap().Num());
        }
    }
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

UClass* UFortGameData::GetSharedMissionListsClass() const
{
    if (Member_SharedMissionListsClassPath.IsAvailable())
    {
        return Reflection::AccessMember<TAssetSubclassOf<UObject>>(this, Member_SharedMissionListsClassPath).LoadSynchronous();
    }
    return Member_SharedMissionListsClass.IsAvailable() ? static_cast<UClass*>(SharedMissionListsClass) : nullptr;
}
