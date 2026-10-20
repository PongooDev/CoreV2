#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Engine/World.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "Hooks.h"
#include "FortKismetLibrary.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortQuickBars.h"
#include "Finder.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

bool AFortPlayerController::InitializeMcpProfile(const void* OnComplete)
{
    const bool bInitialized = InitializeMcpProfileOG(this, OnComplete);

#if WITH_SERVER_CODE
    if (Role == ROLE_Authority && !QuickBars)
    {
        SpawnQuickBars();
        AddDefaultInventory();
    }
#endif

    return bInitialized;
}

void AFortPlayerController::SpawnQuickBars()
{
#if WITH_SERVER_CODE
    FActorSpawnParameters SpawnInfo;
    SpawnInfo.Owner = this;
    QuickBars = GetWorld()->SpawnActor<AFortQuickBars>(AFortQuickBars::StaticClass(), SpawnInfo);
    SetupQuickBars();
#endif
}

void AFortPlayerController::SetupQuickBars()
{
#if WITH_SERVER_CODE
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (!QuickBars || !GameData)
    {
        return;
    }

    QuickBars->SetupQuickbar(EFortQuickBars::Primary, GameData->GetQuickBarDefinition(EFortQuickBars::Primary));
    QuickBars->SetupQuickbar(EFortQuickBars::Secondary, GameData->GetQuickBarDefinition(EFortQuickBars::Secondary));
#endif
}

void AFortPlayerController::AddDefaultInventory()
{
#if WITH_SERVER_CODE
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (!GameData || !WorldInventory)
    {
        return;
    }

    TStructArray<FItemDefinitionAndCount>& DefaultInventoryList = GameData->DefaultInventoryList;
    for (int32_t Index = 0; Index < DefaultInventoryList.Num(); ++Index)
    {
        const FItemDefinitionAndCount& Entry = DefaultInventoryList[Index];
        const TAssetPtr<UFortItemDefinition>& EntryDefinition = Entry.ItemDefinition;
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(EntryDefinition.LoadSynchronous());
        if (ItemDefinition)
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(this, ItemDefinition, Entry.Count, false);
        }
    }
#endif
}

void AFortPlayerController::TryAddToQuickBar(UFortWorldItem* Item)
{
#if WITH_SERVER_CODE
    if (!QuickBars || !Item)
    {
        UE_LOG(LogFort, Warning, TEXT("Failed TryAddToQuickBar due to invalid QuickBars '%s' or Item '%s'"), *GetNameSafe(static_cast<AFortQuickBars*>(QuickBars)), *GetNameSafe(Item));
        return;
    }

    FFortItemEntry& ItemEntry = Item->ItemEntry;
    UFortItemDefinition* ItemDefinition = ItemEntry.ItemDefinition;
    if (!ItemDefinition)
    {
        UE_LOG(LogFort, Warning, TEXT("Failed TryAddToQuickBar due to invalid ItemDefinition for Item '%s'"), *GetNameSafe(Item));
        return;
    }

    const FGuid ItemGuid = ItemEntry.ItemGuid;
    AFortQuickBars* Bars = QuickBars;
    if (Bars->ContainsItem(ItemGuid))
    {
        UE_LOG(LogFort, Log, TEXT("TryAddToQuickBar did not add item '%s' with definition '%s' because it is already on the quickbar"), *GetNameSafe(Item), *GetNameSafe(ItemDefinition));
        return;
    }

    const uint8_t ItemType = ItemDefinition->ItemType;
    for (EFortQuickBars Bar : { EFortQuickBars::Primary, EFortQuickBars::Secondary })
    {
        const int32_t NumSlots = Bars->GetQuickBar(Bar).Slots.Num();
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            const TArray<uint8_t>& AcceptedItemTypes = Bars->GetSlotData(Bar, SlotIndex).AcceptedItemTypes;
            if (AcceptedItemTypes.Contains(ItemType) && Bars->GetSlot(Bar, SlotIndex).Items.Num() == 0)
            {
                Bars->ServerAddItemInternal(ItemGuid, Bar, SlotIndex);
                return;
            }
        }
    }
#endif
}

void AFortPlayerController::ServerExecuteInventoryItem_Implementation(const FGuid& ItemGuid)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    if (UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition)))
    {
        ItemDefinition->ServerExecute(WorldItem, this);
    }
#endif
}

void AFortPlayerController::HookVTables()
{
    const int32_t ExecuteIndex = Finder::ImplementationIndex(StaticFunction_ServerExecuteInventoryItem());
    if (ExecuteIndex < 0)
    {
        Log::Error(L"Finder: AFortPlayerController::ServerExecuteInventoryItem_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[ExecuteIndex] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPlayerController::ServerExecuteInventoryItem_Implementation", VTable, ExecuteIndex, reinterpret_cast<void*>(&ServerExecuteInventoryItem_ImplementationHK));
        }
    }
}

void AFortPlayerController::ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid)
{
    This->ServerExecuteInventoryItem_Implementation(ItemGuid);
}

void AFortPlayerController::Hook()
{
    Hooks::Detour(L"AFortPlayerController::InitializeMcpProfile", reinterpret_cast<void*>(Natives::AFortPlayerController_InitializeMcpProfile), &InitializeMcpProfileHK, &InitializeMcpProfileOG);
}

bool AFortPlayerController::InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete)
{
    return This->InitializeMcpProfile(OnComplete);
}
