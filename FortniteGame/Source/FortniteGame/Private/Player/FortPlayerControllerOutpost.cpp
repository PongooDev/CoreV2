#include "pch.h"
#include "Player/FortPlayerControllerOutpost.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Online/FortMcpProfileWorld.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace
{
    class EOutpostBuildings
    {
        DefineUnrealEnum(EOutpostBuildings, FortniteGame)
        DefineUEnumValue(StormShield)
        DefineUEnumValue(CraftingTable)
        DefineUEnumValue(Fabricator)
        DefineUEnumValue(HarvestingOptimizer)
        DefineUEnumValue(StorageVault)
        DefineUEnumValue(POST)
    };

    class UFortOutpostFunctionLibrary : public UObject
    {
        DefineUnrealClass(UFortOutpostFunctionLibrary, FortniteGame)
        DefineUFunction(GetStorageVaultCapacity)
        DefineUFunction(GetStorageVaultNumItems)
        DefineUFunction(FabricatorCanDisintegrateItem)
        DefineUFunction(GetGooRequiredToFabricateItem)
        DefineUFunction(GetFabricatorStoredGooAmount)
        DefineUFunction(GetFabricatorRecipes)
        DefineUFunction(GetOutpostBuildingItemDefinition)

    public:
        static int32_t GetStorageVaultCapacity(AFortPlayerControllerOutpost* Player)
        {
            Reflection::FParams Params(StaticFunction_GetStorageVaultCapacity());
            Params.Set(L"Player", Player);
            return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<int32_t>(L"ReturnValue") : 0;
        }

        static int32_t GetStorageVaultNumItems(AFortPlayerControllerOutpost* Player)
        {
            Reflection::FParams Params(StaticFunction_GetStorageVaultNumItems());
            Params.Set(L"Player", Player);
            return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<int32_t>(L"ReturnValue") : -1;
        }

        static int32_t FabricatorCanDisintegrateItem(AFortPlayerControllerOutpost* FortPC, UFortWorldItem* Item, int32_t Count)
        {
            Reflection::FParams Params(StaticFunction_FabricatorCanDisintegrateItem());
            Params.Set(L"FortPC", FortPC);
            Params.Set(L"Item", Item);
            Params.Set(L"Count", Count);
            return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<int32_t>(L"ReturnValue") : 0;
        }

        static int32_t GetGooRequiredToFabricateItem(const UFortWorldItem* Item)
        {
            Reflection::FParams Params(StaticFunction_GetGooRequiredToFabricateItem());
            Params.Set(L"Item", Item);
            return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<int32_t>(L"ReturnValue") : 0;
        }

        static int32_t GetFabricatorStoredGooAmount(AFortPlayerControllerOutpost* FortPC)
        {
            Reflection::FParams Params(StaticFunction_GetFabricatorStoredGooAmount());
            Params.Set(L"FortPC", FortPC);
            return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<int32_t>(L"ReturnValue") : 0;
        }

        static TArray<UFortItem*> GetFabricatorRecipes(AFortPlayerControllerOutpost* FortPC)
        {
            Reflection::FParams Params(StaticFunction_GetFabricatorRecipes(), true);
            Params.Set(L"FortPC", FortPC);
            Params.Set(L"InventoryFilter", EFortInventoryFilter::GetMax_None());
            return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<TArray<UFortItem*>>(L"ReturnValue") : TArray<UFortItem*>();
        }

        static UObject* GetOutpostBuildingItemDefinition(UObject* WorldContextObject, EOutpostBuildings Building)
        {
            Reflection::FParams Params(StaticFunction_GetOutpostBuildingItemDefinition());
            Params.Set(L"WorldContextObject", WorldContextObject);
            Params.Set(L"Building", Building);
            return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<UObject*>(L"ReturnValue") : nullptr;
        }
    };

    FString McpItemId(const FGuid& Guid)
    {
        wchar_t Buffer[40] = {};
        swprintf(Buffer, 40, L"%08X-%04X-%04X-%04X-%04X%08X", Guid.A, Guid.B >> 16, Guid.B & 0xFFFF, Guid.C >> 16, Guid.C & 0xFFFF, Guid.D);
        return FString(Buffer);
    }

    std::unordered_map<const UObject*, std::vector<std::pair<FString, int32_t>>> PendingPOSTDeposits;
    void (*OnAccountInventoryChangedOG)(UFortRegisteredPlayerInfo* This, const void* ChangedItemTypes) = nullptr;

    UFortMcpProfileWorld* WorldProfileOf(AFortPlayerController* PlayerController)
    {
        UFortRegisteredPlayerInfo* PlayerInfo = static_cast<UFortRegisteredPlayerInfo*>(PlayerController->MyPlayerInfo);
        return PlayerInfo ? static_cast<UFortMcpProfileWorld*>(PlayerInfo->WorldProfile) : nullptr;
    }
}

void AFortPlayerControllerOutpost::ServerLeaveOutpostAsParty_Implementation()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameModeZone* GameMode = World ? Cast<AFortGameModeZone>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    if (!GameMode || !Finder::ResolveLeaveOutpostLive())
    {
        return;
    }

    Natives::AFortGameMode_SendStayWithParty(GameMode);
    CallVirtual<void>(GameMode, Natives::AGameMode_ReturnToMainMenuHostIndex);
#endif
}

void AFortPlayerControllerOutpost::ServeMoveItemToStorageVault_Implementation(const FFortItemEntry& NewlyStoredItem)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortInventory* Vault = OutpostInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(NewlyStoredItem.ItemGuid) : nullptr;
    if (!WorldItem || !Vault)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    const int32_t Count = std::min(static_cast<int32_t>(NewlyStoredItem.Count), static_cast<int32_t>(ItemEntry.Count));
    if (!ItemDefinition || !ItemDefinition->bCanBeDepositedInStorageVault || Count <= 0)
    {
        return;
    }

    const int32_t FreeSlots = std::max(UFortOutpostFunctionLibrary::GetStorageVaultCapacity(this) - UFortOutpostFunctionLibrary::GetStorageVaultNumItems(this), 0);
    const int32_t Stored = Vault->AddWorldItemCopy(this, ItemEntry, Count, FreeSlots);
    if (Stored <= 0)
    {
        UE_LOG(LogFort, Log, TEXT("ServeMoveItemToStorageVault: no room in the storage vault for %s"), *GetNameSafe(ItemDefinition));
        return;
    }
    RemoveInventoryItem(NewlyStoredItem.ItemGuid, Stored, false);
#endif
}

void AFortPlayerControllerOutpost::ServeMoveItemFromStorageVault_Implementation(const FFortItemEntry& NewlyCollectedItem)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortInventory* Vault = OutpostInventory;
    UFortWorldItem* StoredItem = Vault ? Vault->FindWorldItem(NewlyCollectedItem.ItemGuid) : nullptr;
    if (!StoredItem || !Inventory)
    {
        return;
    }

    FFortItemEntry& ItemEntry = StoredItem->ItemEntry;
    const int32_t Count = std::min(static_cast<int32_t>(NewlyCollectedItem.Count), static_cast<int32_t>(ItemEntry.Count));
    if (Count <= 0)
    {
        return;
    }

    std::vector<UFortWorldItem*> NewItems;
    const int32_t Collected = Inventory->AddWorldItemCopy(this, ItemEntry, Count, -1, &NewItems);
    for (UFortWorldItem* NewItem : NewItems)
    {
        TryAddToQuickBar(NewItem);
    }
    Vault->RemoveWorldItemCount(StoredItem, Collected);
#endif
}

void AFortPlayerControllerOutpost::ServerDisintegrateItem_Implementation(const FGuid& ItemGuid, int32_t Count)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    UFortMcpProfileWorld* Profile = WorldProfileOf(this);
    const int32_t Allowed = WorldItem ? UFortOutpostFunctionLibrary::FabricatorCanDisintegrateItem(this, WorldItem, Count) : 0;
    if (!Profile || Allowed <= 0)
    {
        return;
    }

    TArray<FString> ItemIds;
    TArray<int32_t> ItemCounts;
    ItemIds.Add(McpItemId(ItemGuid));
    ItemCounts.Add(Allowed);
    Profile->DisintegrateItems(ItemIds, ItemCounts, this, &DisintegrateItems_Completed);
#endif
}

void AFortPlayerControllerOutpost::ServerFabricateItem_Implementation(const UFortWorldItemDefinition* ItemDef)
{
#if WITH_SERVER_CODE
    UFortMcpProfileWorld* Profile = WorldProfileOf(this);
    UFortWorldItemDefinition* PendingItem = ItemPendingToFabricate;
    if (!ItemDef || !Profile || PendingItem)
    {
        return;
    }

    UFortWorldItem* Recipe = nullptr;
    TArray<UFortItem*> Recipes = UFortOutpostFunctionLibrary::GetFabricatorRecipes(this);
    for (int32_t Index = 0; Index < Recipes.Num() && !Recipe; ++Index)
    {
        UFortWorldItem* Candidate = Cast<UFortWorldItem>(Recipes[Index]);
        FFortItemEntry* CandidateEntry = Candidate ? &static_cast<FFortItemEntry&>(Candidate->ItemEntry) : nullptr;
        Recipe = CandidateEntry && static_cast<UFortItemDefinition*>(CandidateEntry->ItemDefinition) == ItemDef ? Candidate : nullptr;
    }
    const int32_t GooRequired = Recipe ? UFortOutpostFunctionLibrary::GetGooRequiredToFabricateItem(Recipe) : 0;
    if (!Recipe || GooRequired <= 0 || GooRequired > UFortOutpostFunctionLibrary::GetFabricatorStoredGooAmount(this))
    {
        UE_LOG(LogFort, Log, TEXT("ServerFabricateItem: %s can't be fabricated (goo required %i)"), *GetNameSafe(ItemDef), GooRequired);
        return;
    }

    ItemPendingToFabricate = const_cast<UFortWorldItemDefinition*>(ItemDef);
    if (!Profile->FabricateItem(ItemDef->GetPersistentName(), this, &FabricateItem_Completed))
    {
        ItemPendingToFabricate = nullptr;
    }
#endif
}

void AFortPlayerControllerOutpost::ServerDepositPOSTResource_Implementation(const FGuid& ItemGuid, int32_t Count)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    UFortMcpProfileWorld* Profile = WorldProfileOf(this);
    if (!WorldItem || !Profile || !IsOutpostOwner())
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const int32_t Deposited = std::min(Count, static_cast<int32_t>(ItemEntry.Count));
    UFortItemDefinition* ItemDefinition = ItemEntry.ItemDefinition;
    if (Deposited <= 0 || !ItemDefinition)
    {
        return;
    }

    TArray<FString> ItemIds;
    TArray<int32_t> ItemCounts;
    ItemIds.Add(McpItemId(ItemGuid));
    ItemCounts.Add(Deposited);
    PendingPOSTDeposits[this].emplace_back(ItemDefinition->GetPersistentName(), Deposited);
    if (!Profile->DepositPostResources(ItemIds, ItemCounts, this, &DepositPOSTResource_Completed))
    {
        PendingPOSTDeposits[this].pop_back();
    }
#endif
}

void AFortPlayerControllerOutpost::ServerUpgradePOST_Implementation()
{
#if WITH_SERVER_CODE
    UObject* POST = IsOutpostOwner() ? UFortOutpostFunctionLibrary::GetOutpostBuildingItemDefinition(this, EOutpostBuildings::GetPOST()) : nullptr;
    if (POST)
    {
        UpgradeBuildingByDefinition(POST);
    }
#endif
}

bool AFortPlayerControllerOutpost::IsOutpostOwner() const
{
    Reflection::FParams Params(StaticFunction_IsOutpostOwner());
    return Params.Call(const_cast<AFortPlayerControllerOutpost*>(this)) && Params.Take<bool>(L"ReturnValue");
}

void AFortPlayerControllerOutpost::UpgradeBuildingByDefinition(UObject* OutpostBuilding)
{
    Reflection::FParams Params(StaticFunction_UpgradeBuildingByDefinition());
    Params.Set(L"OutpostBuilding", OutpostBuilding);
    Params.Call(this);
}

int32_t AFortPlayerControllerOutpost::GetBuildingLevelFromDefinition(UObject* OutpostBuilding) const
{
    Reflection::FParams Params(StaticFunction_GetBuildingLevelFromDefinition());
    Params.Set(L"OutpostBuilding", OutpostBuilding);
    return Params.Call(const_cast<AFortPlayerControllerOutpost*>(this)) ? Params.Take<int32_t>(L"ReturnValue") : 0;
}

void AFortPlayerControllerOutpost::ClientOnBuildingLevelChanged(UObject* UpgradedBuilding, int32_t InLevelBeforeUpgrade)
{
    Reflection::FParams Params(StaticFunction_ClientOnBuildingLevelChanged());
    Params.Set(L"UpgradedBuilding", UpgradedBuilding);
    Params.Set(L"InLevelBeforeUpgrade", InLevelBeforeUpgrade);
    Params.Call(this);
}

void AFortPlayerControllerOutpost::OnAccountInventoryChanged()
{
#if WITH_SERVER_CODE
    UObject* UpgradedBuilding = BuildingBeingUpgraded;
    const int32_t PreviousLevel = LevelBeforeUpgrade;
    if (Role != ROLE_Authority || !UpgradedBuilding || GetBuildingLevelFromDefinition(UpgradedBuilding) <= PreviousLevel)
    {
        return;
    }

    BuildingBeingUpgraded = nullptr;
    UE_LOG(LogFort, Log, TEXT("AFortPlayerControllerOutpost::OnAccountInventoryChanged: %s upgraded from level %d"), *GetNameSafe(UpgradedBuilding), PreviousLevel);
    ClientOnBuildingLevelChanged(UpgradedBuilding, PreviousLevel);
#endif
}

void AFortPlayerControllerOutpost::UpdateFabricatorProgress()
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Finder::ResolveOutpostMcpRequestLive())
    {
        return;
    }

    int64_t StartTime = 0;
    Natives::FDateTime_UtcNow(&StartTime);
    Natives::AFortPlayerControllerOutpost_GetFabricatorProgress(this, &StartTime, &GetMember_QuantumGooCompleted(), &GetMember_QuantumGooIncoming());
    DisintegrationStartTime = StartTime;
    ForceNetUpdate();
#endif
}

void AFortPlayerControllerOutpost::DisintegrateItems_Completed(UObject* Object, const void* Result)
{
    AFortPlayerControllerOutpost* This = static_cast<AFortPlayerControllerOutpost*>(Object);
    UE_LOG(LogFort, Log, TEXT("AFortPlayerControllerOutpost::DisintegrateItems_Completed: bSuccess %i"), *static_cast<const uint8_t*>(Result));
    This->UpdateFabricatorProgress();
}

void AFortPlayerControllerOutpost::FabricateItem_Completed(UObject* Object, const void* Result)
{
    AFortPlayerControllerOutpost* This = static_cast<AFortPlayerControllerOutpost*>(Object);
    const bool bSuccess = *static_cast<const uint8_t*>(Result) != 0;
    UE_LOG(LogFort, Log, TEXT("AFortPlayerControllerOutpost::FabricateItem_Completed: bSuccess %i"), bSuccess);
    This->UpdateFabricatorProgress();

    UFortWorldItemDefinition* Fabricated = This->ItemPendingToFabricate;
    This->ItemPendingToFabricate = nullptr;
    AFortInventory* Inventory = This->WorldInventory;
    if (!bSuccess || !Fabricated || !Inventory)
    {
        return;
    }

    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = Instances.Num() - 1; Index >= 0; --Index)
    {
        UFortWorldItem* WorldItem = Instances[Index];
        FFortItemEntry* ItemEntry = WorldItem ? &static_cast<FFortItemEntry&>(WorldItem->ItemEntry) : nullptr;
        if (ItemEntry && static_cast<UFortItemDefinition*>(ItemEntry->ItemDefinition) == Fabricated)
        {
            This->FabricatedItemPendingForNotification = WorldItem;
            This->TryAddToQuickBar(WorldItem);
            return;
        }
    }
}

void AFortPlayerControllerOutpost::DepositPOSTResource_Completed(UObject* Object, const void* Result)
{
    AFortPlayerControllerOutpost* This = static_cast<AFortPlayerControllerOutpost*>(Object);
    const bool bSuccess = *static_cast<const uint8_t*>(Result) != 0;
    UE_LOG(LogFort, Log, TEXT("AFortPlayerControllerOutpost::DepositPOSTResource_Completed: bSuccess %i"), bSuccess);

    std::vector<std::pair<FString, int32_t>>& Pending = PendingPOSTDeposits[This];
    if (Pending.empty())
    {
        return;
    }
    const auto [TemplateId, Deposited] = Pending.front();
    Pending.erase(Pending.begin());
    if (!bSuccess)
    {
        return;
    }

    TArray<FFortDepositedResources>& Resources = This->PostDepositedResources;
    int32_t Index = 0;
    while (Index < Resources.Num() && Resources[Index].TemplateId.ToStdString() != TemplateId.ToStdString())
    {
        ++Index;
    }
    if (Index == Resources.Num())
    {
        Resources.Add(FFortDepositedResources{ TemplateId, 0 });
    }
    Resources[Index].Quantity += Deposited;
    This->ForceNetUpdate();
}

void AFortPlayerControllerOutpost::Hook()
{
    Hooks::Detour(L"UFortRegisteredPlayerInfo::OnAccountInventoryChanged", reinterpret_cast<void*>(Natives::UFortRegisteredPlayerInfo_OnAccountInventoryChanged), &OnAccountInventoryChangedHK, &OnAccountInventoryChangedOG);
}

void AFortPlayerControllerOutpost::OnAccountInventoryChangedHK(UFortRegisteredPlayerInfo* PlayerInfo, const void* ChangedItemTypes)
{
    OnAccountInventoryChangedOG(PlayerInfo, ChangedItemTypes);

#if WITH_SERVER_CODE
    for (TObjectIterator<AFortPlayerControllerOutpost> It; It; ++It)
    {
        if (static_cast<UFortRegisteredPlayerInfo*>(It->MyPlayerInfo) == PlayerInfo)
        {
            It->OnAccountInventoryChanged();
        }
    }
#endif
}

void AFortPlayerControllerOutpost::HookVTables()
{
    for (const auto& [Function, Implementation, Name] : {
             std::tuple{ StaticFunction_ServerLeaveOutpostAsParty(), reinterpret_cast<void*>(&ServerLeaveOutpostAsParty_ImplementationHK), L"AFortPlayerControllerOutpost::ServerLeaveOutpostAsParty_Implementation" },
             std::tuple{ FindStaticFunction_ServeMoveItemToStorageVault() ? StaticFunction_ServeMoveItemToStorageVault() : StaticFunction_ServerMoveItemToStorageVault(), reinterpret_cast<void*>(&ServeMoveItemToStorageVault_ImplementationHK), L"AFortPlayerControllerOutpost::ServeMoveItemToStorageVault_Implementation" },
             std::tuple{ FindStaticFunction_ServeMoveItemFromStorageVault() ? StaticFunction_ServeMoveItemFromStorageVault() : StaticFunction_ServerMoveItemFromStorageVault(), reinterpret_cast<void*>(&ServeMoveItemFromStorageVault_ImplementationHK), L"AFortPlayerControllerOutpost::ServeMoveItemFromStorageVault_Implementation" },
             std::tuple{ StaticFunction_ServerDisintegrateItem(), reinterpret_cast<void*>(&ServerDisintegrateItem_ImplementationHK), L"AFortPlayerControllerOutpost::ServerDisintegrateItem_Implementation" },
             std::tuple{ StaticFunction_ServerFabricateItem(), reinterpret_cast<void*>(&ServerFabricateItem_ImplementationHK), L"AFortPlayerControllerOutpost::ServerFabricateItem_Implementation" },
             std::tuple{ StaticFunction_ServerDepositPOSTResource(), reinterpret_cast<void*>(&ServerDepositPOSTResource_ImplementationHK), L"AFortPlayerControllerOutpost::ServerDepositPOSTResource_Implementation" },
             std::tuple{ StaticFunction_ServerUpgradePOST(), reinterpret_cast<void*>(&ServerUpgradePOST_ImplementationHK), L"AFortPlayerControllerOutpost::ServerUpgradePOST_Implementation" },
         })
    {
        const int32_t Index = Finder::ImplementationIndex(Function);
        if (Index < 0)
        {
            Log::Error(L"Finder: {} slot not found", Name);
            continue;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
            {
                Hooks::SwapVTable(Name, VTable, Index, Implementation);
            }
        }
    }
}

void AFortPlayerControllerOutpost::ServerLeaveOutpostAsParty_ImplementationHK(AFortPlayerControllerOutpost* This)
{
    This->ServerLeaveOutpostAsParty_Implementation();
}

void AFortPlayerControllerOutpost::ServeMoveItemToStorageVault_ImplementationHK(AFortPlayerControllerOutpost* This, const FFortItemEntry& NewlyStoredItem)
{
    This->ServeMoveItemToStorageVault_Implementation(NewlyStoredItem);
}

void AFortPlayerControllerOutpost::ServeMoveItemFromStorageVault_ImplementationHK(AFortPlayerControllerOutpost* This, const FFortItemEntry& NewlyCollectedItem)
{
    This->ServeMoveItemFromStorageVault_Implementation(NewlyCollectedItem);
}

void AFortPlayerControllerOutpost::ServerDisintegrateItem_ImplementationHK(AFortPlayerControllerOutpost* This, const FGuid& ItemGuid, int32_t Count)
{
    This->ServerDisintegrateItem_Implementation(ItemGuid, Count);
}

void AFortPlayerControllerOutpost::ServerFabricateItem_ImplementationHK(AFortPlayerControllerOutpost* This, const UFortWorldItemDefinition* ItemDef)
{
    This->ServerFabricateItem_Implementation(ItemDef);
}

void AFortPlayerControllerOutpost::ServerDepositPOSTResource_ImplementationHK(AFortPlayerControllerOutpost* This, const FGuid& ItemGuid, int32_t Count)
{
    This->ServerDepositPOSTResource_Implementation(ItemGuid, Count);
}

void AFortPlayerControllerOutpost::ServerUpgradePOST_ImplementationHK(AFortPlayerControllerOutpost* This)
{
    This->ServerUpgradePOST_Implementation();
}
