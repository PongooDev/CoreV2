#include "pch.h"
#include "Items/FortLootTablesCert.h"
#include "Engine/DataTable.h"
#include "FortGameData.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortLootPackage.h"
#include "Log.h"
#include "UObject/UObjectGlobals.h"

#include <string>

namespace
{
    struct FCertLootTierRow
    {
        const wchar_t* Name;
        const wchar_t* TierGroup;
        float Weight;
        const wchar_t* QuotaLevel;
        int32_t LootTier;
        int32_t MinWorldLevel;
        int32_t MaxWorldLevel;
        const wchar_t* StreakBreakerCurrency;
        int32_t StreakBreakerPointsMin;
        int32_t StreakBreakerPointsMax;
        int32_t StreakBreakerPointsSpend;
        const wchar_t* LootPackage;
        const wchar_t* LootPreviewPackage;
        float NumLootPackageDrops;
        int32_t LootPackageCategoryWeightArray[11];
        int32_t LootPackageCategoryMinArray[11];
        int32_t LootPackageCategoryMaxArray[11];
        const wchar_t* RequiredGameplayTag;
        bool bAllowBonusLootDrops;
    };

    struct FCertLootPackageRow
    {
        const wchar_t* Name;
        const wchar_t* LootPackageID;
        float Weight;
        int32_t Count;
        int32_t LootPackageCategory;
        const wchar_t* RequiredTag;
        const wchar_t* LootPackageCall;
        const wchar_t* ItemDefinition;
        const wchar_t* PersistentLevel;
        int32_t MinWorldLevel;
        int32_t MaxWorldLevel;
        bool bAllowBonusDrops;
    };

    struct FCertLootQuotaRow
    {
        const wchar_t* Name;
        const wchar_t* QuotaCategory;
        const wchar_t* QuotaLevel;
        int32_t Min;
        int32_t Max;
        float Quota;
        int32_t MinWorldLevel;
        int32_t MaxWorldLevel;
    };

#include "FortLootTablesCertData.inl"

    constexpr const wchar_t* LootTierDataPath = L"/Game/Items/Datatables/LootTierData_Client.LootTierData_Client";
    constexpr const wchar_t* LootPackagesPath = L"/Game/Items/Datatables/LootPackages_Client.LootPackages_Client";
    constexpr const wchar_t* LootQuotaDataPath = L"/Game/Items/Datatables/LootQuotaData.LootQuotaData";

    FName NameOf(const wchar_t* Text)
    {
        return std::wstring_view(Text) == L"None" ? FName() : FName(Text);
    }

    TArray<int32_t> ArrayOf(const int32_t (&Values)[11])
    {
        TArray<int32_t> Array;
        for (int32_t Value : Values)
        {
            Array.Add(Value);
        }
        return Array;
    }

    ELootQuotaLevel QuotaLevelOf(const wchar_t* Name)
    {
        return ELootQuotaLevel{ ELootQuotaLevel::StaticEnumInfo().FindValue(Name) };
    }

    UDataTable* CreateTable(const wchar_t* ObjectPath, UScriptStruct* RowStruct)
    {
        const std::wstring Path(ObjectPath);
        const size_t Dot = Path.find(L'.');
        const std::wstring PackageName = Path.substr(0, Dot);
        const std::wstring ObjectName = Path.substr(Dot + 1);

        UClass* PackageClass = FindObject<UClass>(nullptr, L"/Script/CoreUObject.Package");
        UObject* Package = StaticFindObject(PackageClass, nullptr, PackageName.c_str());
        if (!Package && PackageClass)
        {
            Package = StaticConstructObject_Internal(PackageClass, nullptr, FName(PackageName.c_str()), RF_Public, EInternalObjectFlags::RootSet);
        }
        if (!Package || !RowStruct)
        {
            Log::Error(L"FortLootTablesCert: could not create {}", Path);
            return nullptr;
        }

        UDataTable* Table = static_cast<UDataTable*>(StaticConstructObject_Internal(UDataTable::StaticClass(), Package, FName(ObjectName.c_str()), static_cast<EObjectFlags>(RF_Public | RF_Standalone), EInternalObjectFlags::RootSet));
        if (Table)
        {
            Table->RowStruct = RowStruct;
        }
        return Table;
    }

    UDataTable* CreateTierTable()
    {
        UDataTable* Table = CreateTable(LootTierDataPath, FFortLootTierData::StaticStruct());
        if (!Table)
        {
            return nullptr;
        }
        for (const FCertLootTierRow& Source : CertLootTierRows)
        {
            FFortLootTierData* Row = reinterpret_cast<FFortLootTierData*>(Table->AddRow(FName(Source.Name)));
            Row->TierGroup = NameOf(Source.TierGroup);
            Row->Weight = Source.Weight;
            Row->QuotaLevel = QuotaLevelOf(Source.QuotaLevel);
            Row->LootTier = Source.LootTier;
            Row->MinWorldLevel = Source.MinWorldLevel;
            Row->MaxWorldLevel = Source.MaxWorldLevel;
            Row->StreakBreakerCurrency = FString(Source.StreakBreakerCurrency);
            Row->StreakBreakerPointsMin = Source.StreakBreakerPointsMin;
            Row->StreakBreakerPointsMax = Source.StreakBreakerPointsMax;
            Row->StreakBreakerPointsSpend = Source.StreakBreakerPointsSpend;
            Row->LootPackage = NameOf(Source.LootPackage);
            Row->LootPreviewPackage = NameOf(Source.LootPreviewPackage);
            Row->NumLootPackageDrops = Source.NumLootPackageDrops;
            Row->LootPackageCategoryWeightArray = ArrayOf(Source.LootPackageCategoryWeightArray);
            Row->LootPackageCategoryMinArray = ArrayOf(Source.LootPackageCategoryMinArray);
            Row->LootPackageCategoryMaxArray = ArrayOf(Source.LootPackageCategoryMaxArray);
            Row->RequiredGameplayTag = FGameplayTag{ NameOf(Source.RequiredGameplayTag) };
            Row->bAllowBonusLootDrops = Source.bAllowBonusLootDrops;
        }
        return Table;
    }

    UDataTable* CreatePackageTable()
    {
        UDataTable* Table = CreateTable(LootPackagesPath, FFortLootPackageData::StaticStruct());
        if (!Table)
        {
            return nullptr;
        }
        for (const FCertLootPackageRow& Source : CertLootPackageRows)
        {
            FFortLootPackageData* Row = reinterpret_cast<FFortLootPackageData*>(Table->AddRow(FName(Source.Name)));
            Row->LootPackageID = NameOf(Source.LootPackageID);
            Row->Weight = Source.Weight;
            Row->Count = Source.Count;
            Row->LootPackageCategory = Source.LootPackageCategory;
            Row->RequiredTag = NameOf(Source.RequiredTag);
            Row->LootPackageCall = FString(Source.LootPackageCall);
            Row->ItemDefinition = TAssetPtr<UFortItemDefinition>(FStringAssetReference(FString(Source.ItemDefinition)));
            Row->PersistentLevel = FString(Source.PersistentLevel);
            Row->MinWorldLevel = Source.MinWorldLevel;
            Row->MaxWorldLevel = Source.MaxWorldLevel;
            Row->bAllowBonusDrops = Source.bAllowBonusDrops;
        }
        return Table;
    }

    UDataTable* CreateQuotaTable()
    {
        UDataTable* Table = CreateTable(LootQuotaDataPath, FFortLootQuotaData::StaticStruct());
        if (!Table)
        {
            return nullptr;
        }
        for (const FCertLootQuotaRow& Source : CertLootQuotaRows)
        {
            FFortLootQuotaData* Row = reinterpret_cast<FFortLootQuotaData*>(Table->AddRow(FName(Source.Name)));
            Row->QuotaCategory = NameOf(Source.QuotaCategory);
            Row->QuotaLevel = QuotaLevelOf(Source.QuotaLevel);
            Row->Min = Source.Min;
            Row->Max = Source.Max;
            Row->Quota = Source.Quota;
            Row->MinWorldLevel = Source.MinWorldLevel;
            Row->MaxWorldLevel = Source.MaxWorldLevel;
        }
        return Table;
    }

    // True when the game data names the table and it does not load (1.2's pak ships neither of Cert's two tables).
    bool IsMissing(TArray<TAssetPtr<UDataTable>>& Tables, const wchar_t* Path)
    {
        for (TAssetPtr<UDataTable>& Table : Tables)
        {
            if (_wcsicmp(*Table.ToStringReference().ToString(), Path) == 0)
            {
                return !Table.LoadSynchronous();
            }
        }
        return false;
    }
}

void FortLootTablesCert::CreateMissingTables(UFortGameData* GameData)
{
    static bool bChecked = false;
    if (bChecked || !GameData)
    {
        return;
    }
    bChecked = true;

    // All three must be missing: on Cert the real assets load and the embedded rows are never read.
    if (GameData->LootQuotaData || !IsMissing(GameData->LootTierDataTables, LootTierDataPath) || !IsMissing(GameData->LootPackageDataTables, LootPackagesPath))
    {
        return;
    }

    Log::Info(L"Loot tables missing from this build's pak: using the embedded Cert tables (3532353)");
    UDataTable* TierTable = CreateTierTable();
    UDataTable* PackageTable = CreatePackageTable();
    GameData->LootQuotaData = CreateQuotaTable();
    Log::Info(L"Embedded Cert tables: {} ({} rows), {} ({} rows), {} ({} rows)",
        LootTierDataPath, TierTable ? TierTable->GetRowMap().Num() : 0,
        LootPackagesPath, PackageTable ? PackageTable->GetRowMap().Num() : 0,
        LootQuotaDataPath, GameData->LootQuotaData ? static_cast<UDataTable*>(GameData->LootQuotaData)->GetRowMap().Num() : 0);
}
