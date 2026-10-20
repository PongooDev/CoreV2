#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Engine/World.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "Hooks.h"
#include "FortKismetLibrary.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Items/FortQuickBars.h"
#include "Finder.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Quests/FortQuestManager.h"
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
        if (UFortGameData* GameData = UFortAssetManager::Get().GetGameData())
        {
            InitializeInventory(GameData->DefaultInventoryList);
        }
    }
#endif

    return bInitialized;
}

void AFortPlayerController::Possess(APawn* InPawn)
{
    PossessOG(this, InPawn);

#if WITH_SERVER_CODE
    AFortQuickBars* Bars = QuickBars;
    if (Role == ROLE_Authority && Bars && MyFortPawn)
    {
        FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
        Bars->ServerActivateSlotInternal(EFortQuickBars::Primary, PrimaryQuickBar.CurrentFocusedSlot, 0.0f, false);
    }
#endif
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

void AFortPlayerController::InitializeInventory(const TStructArray<FItemDefinitionAndCount>& Items)
{
#if WITH_SERVER_CODE
    if (!WorldInventory)
    {
        return;
    }

    AFortInventory* Inventory = WorldInventory;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        TryAddToQuickBar(Instances[Index]);
    }

    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        const FItemDefinitionAndCount& Entry = Items[Index];
        const TAssetPtr<UFortItemDefinition>& EntryDefinition = Entry.ItemDefinition;
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(EntryDefinition.LoadSynchronous());
        if (ItemDefinition && GetItemQuantity(ItemDefinition) <= 0)
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

void AFortPlayerController::ServerSpawnInventoryDrop_Implementation(const FGuid& ItemGuid, int32_t Count)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    AFortPawn* Pawn = MyFortPawn;
    if (!WorldItem || !Pawn || Count <= 0)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    if (!ItemDefinition || !ItemDefinition->bCanBeDropped)
    {
        return;
    }

    const int32_t DropCount = std::min(Count, static_cast<int32_t>(ItemEntry.Count));
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* DropEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    EntryStruct->InitializeStruct(DropEntry);
    EntryStruct->CopyScriptStruct(DropEntry, &ItemEntry);
    DropEntry->Count = DropCount;

    if (RemoveInventoryItem(ItemGuid, DropCount, false))
    {
        const FVector Location = Pawn->GetTransform().GetLocation();
        if (AFortPickup* Pickup = AFortPickup::CreatePickup(GetWorld(), *DropEntry, &Location, nullptr, this))
        {
            Pickup->TossPickup(Location, Pawn, 0);
        }
    }
    EntryStruct->DestroyStruct(DropEntry);
#endif
}

bool AFortPlayerController::RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
    void* InventoryOwner = reinterpret_cast<uint8_t*>(this) + Natives::AFortPlayerController_InventoryOwnerInterface;
    const bool bRemoved = RemoveInventoryItemOG(InventoryOwner, ItemGuid, Count, bForceRemoveFromQuickBars);

#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return bRemoved;
    }

    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem)
    {
        return false;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const int32_t Remaining = ItemEntry.Count - Count;
    if (Count < 0 || Remaining <= 0)
    {
        if (AFortQuickBars* Bars = QuickBars)
        {
            Bars->ServerRemoveItemInternal(ItemGuid, false, bForceRemoveFromQuickBars);
        }
        Inventory->RemoveWorldItem(WorldItem);
    }
    else
    {
        Inventory->UpdateWorldItemCount(WorldItem, Remaining);
    }
    return true;
#else
    return bRemoved;
#endif
}

void AFortPlayerController::HookVTables()
{
    UObject* PlayerControllerDefault = StaticClass()->GetDefaultObject(false);
    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;

    const int32_t PossessIndex = Finder::ImplementationIndex(AController::StaticFunction_Possess());
    void* const PlayerControllerPossess = PlayerControllerVTable && PossessIndex >= 0 ? PlayerControllerVTable[PossessIndex] : nullptr;
    if (PlayerControllerPossess)
    {
        PossessOG = reinterpret_cast<decltype(PossessOG)>(PlayerControllerPossess);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[PossessIndex] == PlayerControllerPossess)
            {
                Hooks::SwapVTable(L"AFortPlayerController::Possess", VTable, PossessIndex, reinterpret_cast<void*>(&PossessHK));
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::Possess slot not found");
    }
    if (UFunction* ActivateSlotFunction = StaticFunction_ActivateSlot(); ActivateSlotFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        ActivateSlotFunction->SetNativeFunc(&execActivateSlot);
        Log::Info(L"Hook AFortPlayerController::ActivateSlot: native");
    }

    if (Finder::ResolveInventoryOwnerLive(StaticFunction_ServerRemoveInventoryItem(), PlayerControllerVTable))
    {
        const ptrdiff_t InterfaceOffset = Natives::AFortPlayerController_InventoryOwnerInterface;
        const int32_t RemoveIndex = Natives::IFortInventoryOwnerInterface_RemoveInventoryItemIndex;
        void* const PlayerControllerRemove = (*reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(PlayerControllerDefault) + InterfaceOffset))[RemoveIndex];
        RemoveInventoryItemOG = reinterpret_cast<decltype(RemoveInventoryItemOG)>(PlayerControllerRemove);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** InterfaceVTable = ClassDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(ClassDefault) + InterfaceOffset) : nullptr;
            if (InterfaceVTable && InterfaceVTable[RemoveIndex] == PlayerControllerRemove)
            {
                Hooks::SwapVTable(L"AFortPlayerController::RemoveInventoryItem", InterfaceVTable, RemoveIndex, reinterpret_cast<void*>(&RemoveInventoryItemHK));
            }
        }
    }

    for (const auto& [Function, Implementation, Name] : {
             std::tuple{ StaticFunction_ServerExecuteInventoryItem(), reinterpret_cast<void*>(&ServerExecuteInventoryItem_ImplementationHK), L"AFortPlayerController::ServerExecuteInventoryItem_Implementation" },
             std::tuple{ StaticFunction_ServerSpawnInventoryDrop(), reinterpret_cast<void*>(&ServerSpawnInventoryDrop_ImplementationHK), L"AFortPlayerController::ServerSpawnInventoryDrop_Implementation" },
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

int32_t AFortPlayerController::GetItemQuantity(const UFortItemDefinition* ItemDefinition)
{
    AFortInventory* Inventory = WorldInventory;
    if (!Inventory || !ItemDefinition)
    {
        return 0;
    }

    int32_t Quantity = 0;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry& Entry = Instances[Index]->ItemEntry;
        if (static_cast<UFortItemDefinition*>(Entry.ItemDefinition) == ItemDefinition)
        {
            Quantity += Entry.Count;
        }
    }
    return Quantity;
}

UFortQuestManager* AFortPlayerController::GetQuestManager() const
{
    UFortRegisteredPlayerInfo* PlayerInfo = MyPlayerInfo;
    return PlayerInfo ? static_cast<UFortQuestManager*>(PlayerInfo->QuestManager) : nullptr;
}

UFortSchematicItem* AFortPlayerController::GetSchematicForItem(const UFortItemDefinition* ItemDefinition)
{
    if (!Natives::AFortPlayerController_GetSchematicForItem && !Finder::ResolveCraftingLive())
    {
        return nullptr;
    }
    return Natives::AFortPlayerController_GetSchematicForItem(this, ItemDefinition);
}

void AFortPlayerController::ActivateSlot(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot)
{
    if (AFortQuickBars* Bars = QuickBars)
    {
        Bars->ServerActivateSlotInternal(InQuickBar, Slot, ActivateDelay, bUpdatePreviousFocusedSlot);
    }
}

DEFINE_FUNCTION(AFortPlayerController::execActivateSlot)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_InQuickBar);
    P_GET_PROPERTY(UIntProperty, Z_Param_Slot);
    P_GET_PROPERTY(UFloatProperty, Z_Param_ActivateDelay);
    P_GET_UBOOL(Z_Param_bUpdatePreviousFocusedSlot);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->ActivateSlot(static_cast<EFortQuickBars>(Z_Param_InQuickBar), Z_Param_Slot, Z_Param_ActivateDelay, Z_Param_bUpdatePreviousFocusedSlot);
    P_NATIVE_END;
}

AFortPlayerController* AFortPlayerController::FromInventoryOwner(IFortInventoryOwnerInterface* InventoryOwner)
{
    if (!InventoryOwner || Natives::AFortPlayerController_InventoryOwnerInterface < 0)
    {
        return nullptr;
    }
    return reinterpret_cast<AFortPlayerController*>(reinterpret_cast<uint8_t*>(InventoryOwner) - Natives::AFortPlayerController_InventoryOwnerInterface);
}

bool AFortPlayerController::RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
    AFortPlayerController* PlayerController = FromInventoryOwner(static_cast<IFortInventoryOwnerInterface*>(InventoryOwner));
    return PlayerController->RemoveInventoryItem(ItemGuid, Count, bForceRemoveFromQuickBars);
}

void AFortPlayerController::PossessHK(AFortPlayerController* This, APawn* InPawn)
{
    This->Possess(InPawn);
}

void AFortPlayerController::ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid)
{
    This->ServerExecuteInventoryItem_Implementation(ItemGuid);
}

void AFortPlayerController::ServerSpawnInventoryDrop_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, int32_t Count)
{
    This->ServerSpawnInventoryDrop_Implementation(ItemGuid, Count);
}

void AFortPlayerController::Hook()
{
    Hooks::Detour(L"AFortPlayerController::InitializeMcpProfile", reinterpret_cast<void*>(Natives::AFortPlayerController_InitializeMcpProfile), &InitializeMcpProfileHK, &InitializeMcpProfileOG);
}

bool AFortPlayerController::InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete)
{
    return This->InitializeMcpProfile(OnComplete);
}
