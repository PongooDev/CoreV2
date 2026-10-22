#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Abilities/FortAbilityKit.h"
#include "Abilities/FortPlayerAttrSet.h"
#include "AbilitySystemComponent.h"
#include "Engine/World.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortGameState.h"
#include "FortWorldManager.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Hooks.h"
#include "FortKismetLibrary.h"
#include "FortSpottableActorInterface.h"
#include "FortTeamInfo.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Items/FortQuickBars.h"
#include "Finder.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Pawns/FortPlayerPawn.h"
#include "Player/FortPlayerState.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace
{
    struct FHeroAbilityKitItems
    {
        TWeakObjectPtr<AFortPlayerController> PlayerController;
        std::vector<TWeakObjectPtr<UFortItemDefinition>> GrantedDefinitions;
        std::vector<FGuid> PendingQuickBarItems;
    };

    std::unordered_map<const AFortPlayerController*, FHeroAbilityKitItems> HeroAbilityKitItems;

    FHeroAbilityKitItems& GetHeroAbilityKitItems(AFortPlayerController* PlayerController)
    {
        FHeroAbilityKitItems& Items = HeroAbilityKitItems[PlayerController];
        if (Items.PlayerController.Get() != PlayerController)
        {
            std::erase_if(HeroAbilityKitItems, [PlayerController](const auto& Entry) { return Entry.first != PlayerController && !Entry.second.PlayerController.Get(); });
            Items = FHeroAbilityKitItems{ TWeakObjectPtr<AFortPlayerController>(PlayerController) };
        }
        return Items;
    }

    std::vector<UFortWorldItem*> AddHeroAbilityKitItem(AFortPlayerController* PlayerController, AFortInventory* Inventory, UFortWorldItemDefinition* ItemDefinition, int32_t Count, int32_t Level)
    {
        std::vector<UFortWorldItem*> Added;
        const int32_t MaxStackSize = std::max(static_cast<int32_t>(ItemDefinition->MaxStackSize), 1);
        int32_t Remaining = Count;

        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num() && Remaining > 0; ++Index)
        {
            UFortWorldItem* Existing = Instances[Index];
            FFortItemEntry* ExistingEntry = Existing ? &static_cast<FFortItemEntry&>(Existing->ItemEntry) : nullptr;
            if (ExistingEntry && static_cast<UFortItemDefinition*>(ExistingEntry->ItemDefinition) == ItemDefinition && ExistingEntry->Count < MaxStackSize)
            {
                const int32_t Merged = std::min(Remaining, MaxStackSize - ExistingEntry->Count);
                Inventory->UpdateWorldItemCount(Existing, ExistingEntry->Count + Merged);
                Remaining -= Merged;
            }
        }

        while (Remaining > 0)
        {
            const int32_t StackCount = std::min(Remaining, MaxStackSize);
            UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(StackCount, Level));
            if (!WorldItem)
            {
                break;
            }

            WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
            Inventory->AddWorldItem(WorldItem);
            Added.push_back(WorldItem);
            Remaining -= StackCount;
        }
        return Added;
    }
}

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
    AFortPlayerState* FortPlayerState = Role == ROLE_Authority && MyFortPawn ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState))) : nullptr;
    if (FortPlayerState && !FortPlayerState->bHasStartedPlaying)
    {
        FortPlayerState->bHasStartedPlaying = true;
        FortPlayerState->ForceNetUpdate();
    }

    if (AFortPlayerPawn* PlayerPawn = Role == ROLE_Authority ? Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(static_cast<AFortPawn*>(MyFortPawn))) : nullptr)
    {
        PlayerPawn->ResetGameplayAttributes();
    }

    AFortQuickBars* Bars = QuickBars;
    if (Role == ROLE_Authority && Bars && MyFortPawn)
    {
        AddHeroAbilityKitGadgetsToQuickBars();
        ApplyHarvestingTool();

        FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
        Bars->ServerActivateSlotInternal(EFortQuickBars::GetPrimary(), PrimaryQuickBar.CurrentFocusedSlot, 0.0f, false);
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

    QuickBars->SetupQuickbar(EFortQuickBars::GetPrimary(), GameData->GetQuickBarDefinition(EFortQuickBars::GetPrimary()));
    QuickBars->SetupQuickbar(EFortQuickBars::GetSecondary(), GameData->GetQuickBarDefinition(EFortQuickBars::GetSecondary()));
#endif
}

void AFortPlayerController::InitializeInventory(const TStructArray<FItemDefinitionAndCount>& Items)
{
#if WITH_SERVER_CODE
    if (!WorldInventory)
    {
        return;
    }

    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (AFortWorldManager* WorldManager = GameState ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr)
    {
        WorldManager->RestorePlayerQuickBars(this);
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
        if (ItemDefinition && ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest())
        {
            continue;
        }
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

    const EFortItemType ItemType = ItemDefinition->ItemType;
    for (EFortQuickBars Bar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        const int32_t NumSlots = Bars->GetQuickBar(Bar).Slots.Num();
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            const TArray<EFortItemType>& AcceptedItemTypes = Bars->GetSlotData(Bar, SlotIndex).AcceptedItemTypes;
            if (AcceptedItemTypes.Contains(ItemType) && Bars->GetSlot(Bar, SlotIndex).Items.Num() == 0)
            {
                Bars->ServerAddItemInternal(ItemGuid, Bar, SlotIndex);
                return;
            }
        }
    }
#endif
}

void AFortPlayerController::TrySetHeroImplementation(UFortHero* NewHero, bool bSendToServer, bool bUnused)
{
    TrySetHeroImplementationOG(this, NewHero, bSendToServer, bUnused);

#if WITH_SERVER_CODE
    ApplyHeroAbilityKitItems(NewHero);
#endif
}

void AFortPlayerController::ApplyHeroAbilityKitItems(UFortHero* Hero)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(static_cast<APlayerState*>(PlayerState));
    if (Role != ROLE_Authority || !Inventory || !Hero || !FortPlayerState)
    {
        return;
    }

    std::vector<UFortAbilityKit*> Kits;
    const auto AddKit = [&Kits](UFortAbilityKit* Kit)
    {
        if (Kit && std::find(Kits.begin(), Kits.end(), Kit) == Kits.end())
        {
            Kits.push_back(Kit);
        }
    };

    UAbilitySystemComponent* AbilitySystemComponent = FortPlayerState->AbilitySystemComponent;
    if (UFortHeroType* HeroType = Hero->GetHeroTypeBP(); HeroType && AbilitySystemComponent)
    {
        TStructArray<FHeroAbilityKit>& HeroTypeKits = HeroType->AbilityKits;
        for (int32_t Index = 0; Index < HeroTypeKits.Num(); ++Index)
        {
            FHeroAbilityKit& HeroTypeKit = HeroTypeKits[Index];
            if (AbilitySystemComponent->HasAllMatchingGameplayTags(HeroTypeKit.RequiredGPTags))
            {
                const TAssetPtr<UFortAbilityKit>& InherentAbilityKit = HeroTypeKit.InherentAbilityKit;
                AddKit(InherentAbilityKit.LoadSynchronous());
            }
        }
    }
    for (UFortAbilityKit* Kit : Hero->SpecializationAbilityKits)
    {
        AddKit(Kit);
    }

    std::vector<UFortItemDefinition*> Definitions;
    std::vector<UFortWorldItemDefinition*> Gadgets;
    std::vector<const FAbilityKitItem*> KitItems;
    const auto AddDefinition = [&Definitions](UFortItemDefinition* Definition)
    {
        if (!Definition || std::find(Definitions.begin(), Definitions.end(), Definition) != Definitions.end())
        {
            return false;
        }
        Definitions.push_back(Definition);
        return true;
    };
    for (UFortAbilityKit* Kit : Kits)
    {
        for (UFortGadgetItemDefinition* Gadget : Kit->Gadgets)
        {
            if (AddDefinition(Gadget))
            {
                Gadgets.push_back(Gadget);
            }
        }

        TStructArray<FAbilityKitItem>& Items = Kit->Items;
        for (int32_t Index = 0; Index < Items.Num(); ++Index)
        {
            const FAbilityKitItem& KitItem = Items[Index];
            UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(KitItem.Item));
            if (ItemDefinition && ItemDefinition->ItemType != EFortItemType::GetWeaponHarvest() && AddDefinition(ItemDefinition))
            {
                KitItems.push_back(&KitItem);
            }
        }
    }

    FHeroAbilityKitItems& Granted = GetHeroAbilityKitItems(this);
    std::vector<UFortItemDefinition*> PreviousDefinitions;
    for (const TWeakObjectPtr<UFortItemDefinition>& GrantedDefinition : Granted.GrantedDefinitions)
    {
        if (UFortItemDefinition* Definition = GrantedDefinition.Get())
        {
            PreviousDefinitions.push_back(Definition);
        }
    }

    std::vector<FGuid> StaleItems;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        UFortItemDefinition* Definition = Entry ? static_cast<UFortItemDefinition*>(Entry->ItemDefinition) : nullptr;
        if (Definition && std::find(PreviousDefinitions.begin(), PreviousDefinitions.end(), Definition) != PreviousDefinitions.end() && std::find(Definitions.begin(), Definitions.end(), Definition) == Definitions.end())
        {
            StaleItems.push_back(Entry->ItemGuid);
        }
    }
    for (const FGuid& ItemGuid : StaleItems)
    {
        RemoveInventoryItem(ItemGuid, -1, true);
    }

    const auto WasGranted = [&PreviousDefinitions](UFortItemDefinition* Definition)
    {
        return std::find(PreviousDefinitions.begin(), PreviousDefinitions.end(), Definition) != PreviousDefinitions.end();
    };

    const int32_t Level = std::max(Hero->GetLevel(), 1);
    for (UFortWorldItemDefinition* Gadget : Gadgets)
    {
        if (WasGranted(Gadget) || GetItemQuantity(Gadget) > 0)
        {
            continue;
        }
        for (UFortWorldItem* GadgetItem : AddHeroAbilityKitItem(this, Inventory, Gadget, 1, Level))
        {
            FFortItemEntry& GadgetEntry = GadgetItem->ItemEntry;
            Granted.PendingQuickBarItems.push_back(GadgetEntry.ItemGuid);
        }
    }

    for (const FAbilityKitItem* KitItem : KitItems)
    {
        UFortWorldItemDefinition* ItemDefinition = static_cast<UFortWorldItemDefinition*>(static_cast<UFortItemDefinition*>(KitItem->Item));
        if (WasGranted(ItemDefinition))
        {
            continue;
        }

        const int32_t Quantity = KitItem->Quantity;
        const int32_t Owned = GetItemQuantity(ItemDefinition);
        int32_t Count = 0;
        const EFortReplenishmentType Replenishment = KitItem->Replenishment;
        if (Replenishment == EFortReplenishmentType::GetClampMin())
        {
            Count = Quantity - Owned;
        }
        else if (Replenishment == EFortReplenishmentType::GetAdd())
        {
            Count = Quantity;
        }
        else
        {
            Count = Owned > 0 ? 0 : Quantity;
        }
        if (Count > 0)
        {
            AddHeroAbilityKitItem(this, Inventory, ItemDefinition, Count, Level);
        }
    }

    Granted.GrantedDefinitions.clear();
    for (UFortItemDefinition* Definition : Definitions)
    {
        Granted.GrantedDefinitions.emplace_back(Definition);
    }

    if (QuickBars && GetPawn())
    {
        AddHeroAbilityKitGadgetsToQuickBars();
    }
#endif
}

void AFortPlayerController::ApplyHarvestingTool()
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortQuickBars* Bars = QuickBars;
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    UAbilitySystemComponent* AbilitySystemComponent = FortPlayerState ? static_cast<UAbilitySystemComponent*>(FortPlayerState->AbilitySystemComponent) : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (Role != ROLE_Authority || !Inventory || !Bars || !GameData)
    {
        return;
    }

    TArray<TAssetPtr<UFortWorldItemDefinition>>& HarvestingTools = GameData->HarvestingTools;
    if (HarvestingTools.Num() == 0)
    {
        return;
    }

    int32_t Level = 0;
    if (const UFortPlayerAttrSet* PlayerAttrSet = AbilitySystemComponent ? AbilitySystemComponent->GetSet<UFortPlayerAttrSet>() : nullptr)
    {
        const FFortGameplayAttributeData& HarvestingToolAttribute = PlayerAttrSet->HarvestingTool;
        Level = std::clamp(static_cast<int32_t>(HarvestingToolAttribute.GetCurrentValue()), 0, HarvestingTools.Num() - 1);
    }
    UFortWorldItemDefinition* HarvestingTool = HarvestingTools[Level].LoadSynchronous();
    if (!HarvestingTool)
    {
        UE_LOG(LogFort, Warning, TEXT("ApplyHarvestingTool: no harvesting tool for level %d"), Level);
        return;
    }

    bool bHasHarvestingTool = false;
    std::vector<FGuid> ReplacedItems;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        UFortItemDefinition* Definition = Entry ? static_cast<UFortItemDefinition*>(Entry->ItemDefinition) : nullptr;
        if (!Definition || Definition->ItemType != EFortItemType::GetWeaponHarvest())
        {
            continue;
        }

        if (Definition == HarvestingTool)
        {
            bHasHarvestingTool = true;
        }
        else
        {
            ReplacedItems.push_back(Entry->ItemGuid);
        }
    }
    if (ReplacedItems.empty() && bHasHarvestingTool)
    {
        return;
    }

    int32_t QuickBarSlot = -1;
    FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
    TStructArray<FQuickBarSlot>& Slots = PrimaryQuickBar.Slots;
    for (int32_t SlotIndex = 0; SlotIndex < Slots.Num() && QuickBarSlot < 0 && !ReplacedItems.empty(); ++SlotIndex)
    {
        TArray<FGuid>& SlotItems = Slots[SlotIndex].Items;
        for (const FGuid& SlotItem : SlotItems)
        {
            if (SlotItem == ReplacedItems.front())
            {
                QuickBarSlot = SlotIndex;
                break;
            }
        }
    }

    for (const FGuid& ItemGuid : ReplacedItems)
    {
        RemoveInventoryItem(ItemGuid, -1, true);
    }

    if (bHasHarvestingTool)
    {
        return;
    }

    UFortWorldItem* WorldItem = Cast<UFortWorldItem>(HarvestingTool->CreateTemporaryItemInstanceBP(1, 1));
    if (!WorldItem)
    {
        return;
    }

    WorldItem->SetOwningControllerForTemporaryItem(this);
    Inventory->AddWorldItem(WorldItem);
    FFortItemEntry& WorldItemEntry = WorldItem->ItemEntry;
    if (QuickBarSlot >= 0)
    {
        Bars->ServerAddItemInternal(WorldItemEntry.ItemGuid, EFortQuickBars::GetPrimary(), QuickBarSlot);
    }
    else
    {
        TryAddToQuickBar(WorldItem);
    }
#endif
}

void AFortPlayerController::AddHeroAbilityKitGadgetsToQuickBars()
{
#if WITH_SERVER_CODE
    AFortQuickBars* Bars = QuickBars;
    AFortInventory* Inventory = WorldInventory;
    FHeroAbilityKitItems& Granted = GetHeroAbilityKitItems(this);
    const std::vector<FGuid> Pending = std::move(Granted.PendingQuickBarItems);
    Granted.PendingQuickBarItems.clear();
    for (const FGuid& ItemGuid : Pending)
    {
        UFortWorldItem* GadgetItem = Bars && Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
        if (!GadgetItem || Bars->ContainsItem(ItemGuid))
        {
            continue;
        }

        FFortItemEntry& GadgetEntry = GadgetItem->ItemEntry;
        UFortWorldItemDefinition* Gadget = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(GadgetEntry.ItemDefinition));
        FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
        const int32_t PreferredSlot = Gadget ? static_cast<int32_t>(Gadget->PreferredQuickbarSlot) : -1;
        const TStructArray<FQuickBarSlot>& Slots = PrimaryQuickBar.Slots;
        Bars->ServerAddItemInternal(ItemGuid, EFortQuickBars::GetPrimary(), PreferredSlot >= 0 && PreferredSlot < Slots.Num() ? PreferredSlot : -1);
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
    WorldItem->SetDurability(ItemEntry.Durability);
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

void AFortPlayerController::ServerSpotActor_Implementation(AActor* NewlySpottedActor)
{
#if WITH_SERVER_CODE
    UClass* const SpottableActorInterface = UFortSpottableActorInterface::StaticClass();
    if (!NewlySpottedActor || !Finder::ResolveSpottingLive(SpottableActorInterface))
    {
        return;
    }

    IFortSpottableActorInterface* Spottable = static_cast<IFortSpottableActorInterface*>(Natives::UObject_GetInterfaceAddress(NewlySpottedActor, SpottableActorInterface));
    if (!Spottable || !Spottable->CanBeSpottedBy(this))
    {
        return;
    }

    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    if (AFortTeamInfo* Team = FortPlayerState ? static_cast<AFortTeamInfo*>(FortPlayerState->PlayerTeam) : nullptr)
    {
        Team->AddSpottedActor(this, NewlySpottedActor);
    }

    AFortPawn* SpotterPawn = MyFortPawn;
    AFortPawn* SpottedPawn = Cast<AFortPawn>(NewlySpottedActor);
    if (SpotterPawn && SpottedPawn)
    {
        SpotterPawn->TriggerFeedbackEvent(SpottedPawn->SpottedEvent, SpotterPawn, SpottedPawn, 0.f);
    }
#endif
}

void AFortPlayerController::ServerRemoveInventoryStateValue_Implementation(const FGuid& ItemGuid, EFortItemEntryState StateType)
{
    ServerRemoveInventoryStateValue_ImplementationOG(this, ItemGuid, StateType);

#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return;
    }

    for (AFortInventory* Inventory : { static_cast<AFortInventory*>(WorldInventory), static_cast<AFortInventory*>(OutpostInventory) })
    {
        if (UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr)
        {
            static_cast<FFortItemEntry&>(WorldItem->ItemEntry).RemoveStateValue(StateType);
            return;
        }
    }
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
    UFortAmmoItemDefinition* AmmoDefinition = Cast<UFortAmmoItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    if (Count >= 0 && Remaining <= 0 && AmmoDefinition && AmmoDefinition->RegenCooldown > 0.0f)
    {
        Inventory->UpdateWorldItemCount(WorldItem, 0);
    }
    else if (Count < 0 || Remaining <= 0)
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

bool AFortPlayerController::ModDurability(const FGuid& ItemGuid, float DurabilityMod, bool bForceSet)
{
#if WITH_SERVER_CODE
    if (Role == ROLE_Authority && !(DurabilityMod < 0.0f && CheatManager && bInfiniteDurability))
    {
        AFortInventory* Inventory = WorldInventory;
        UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
        UFortWorldItemDefinition* ItemDefinition = WorldItem ? Cast<UFortWorldItemDefinition>(WorldItem->GetItemDefinitionBP()) : nullptr;
        if (!ItemDefinition || !WorldItem->HasDurability())
        {
            return false;
        }

        FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
        const float MaxDurability = ItemDefinition->GetMaxDurability(WorldItem->GetLevel());
        const float NewDurability = std::clamp(bForceSet ? DurabilityMod : ItemEntry.Durability + DurabilityMod, 0.0f, std::max(MaxDurability, 0.0f));
        WorldItem->SetDurability(NewDurability);
        if (NewDurability <= 0.0f)
        {
            HandleItemZeroDurability(ItemGuid);
        }
        return true;
    }
#endif
    void* InventoryOwner = reinterpret_cast<uint8_t*>(this) + Natives::AFortPlayerController_InventoryOwnerInterface;
    return ModDurabilityOG(InventoryOwner, ItemGuid, DurabilityMod, bForceSet);
}

bool AFortPlayerController::HandleItemZeroDurability(const FGuid& ItemGuid)
{
#if WITH_SERVER_CODE
    AFortPawn* Pawn = MyFortPawn;
    AFortWeapon* Weapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    if (Weapon && static_cast<FGuid&>(Weapon->ItemEntryGuid) == ItemGuid)
    {
        Weapon->DestroyedDueToDurability();
    }
    return RemoveInventoryItem(ItemGuid, -1, true);
#else
    return false;
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
    if (UFunction* GivePlayerAmmoFunction = StaticFunction_GivePlayerAmmo(); GivePlayerAmmoFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        GivePlayerAmmoFunction->SetNativeFunc(&execGivePlayerAmmo);
        Log::Info(L"Hook AFortPlayerController::GivePlayerAmmo: native");
    }
    if (UFunction* SwapQuickBarFocusFunction = StaticFunction_SwapQuickBarFocus(); SwapQuickBarFocusFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        SwapQuickBarFocusFunction->SetNativeFunc(&execSwapQuickBarFocus);
        Log::Info(L"Hook AFortPlayerController::SwapQuickBarFocus: native");
    }

    if (Finder::ResolveInventoryOwnerLive(StaticFunction_ServerRemoveInventoryItem(), PlayerControllerDefault, Member_WorldInventory.GetOffsetChecked()))
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

        if (ResolveDurability())
        {
            const int32_t ModIndex = Natives::IFortInventoryOwnerInterface_ModDurabilityIndex;
            void* const PlayerControllerMod = (*reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(PlayerControllerDefault) + InterfaceOffset))[ModIndex];
            ModDurabilityOG = reinterpret_cast<decltype(ModDurabilityOG)>(PlayerControllerMod);
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Class = *It;
                UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
                void** InterfaceVTable = ClassDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(ClassDefault) + InterfaceOffset) : nullptr;
                if (InterfaceVTable && InterfaceVTable[ModIndex] == PlayerControllerMod)
                {
                    Hooks::SwapVTable(L"AFortPlayerController::ModDurability", InterfaceVTable, ModIndex, reinterpret_cast<void*>(&ModDurabilityHK));
                }
            }
        }
    }

    if (const int32_t StateValueIndex = Finder::ImplementationIndex(StaticFunction_ServerRemoveInventoryStateValue()); PlayerControllerVTable && StateValueIndex >= 0)
    {
        void* const PlayerControllerRemoveStateValue = PlayerControllerVTable[StateValueIndex];
        ServerRemoveInventoryStateValue_ImplementationOG = reinterpret_cast<decltype(ServerRemoveInventoryStateValue_ImplementationOG)>(PlayerControllerRemoveStateValue);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[StateValueIndex] == PlayerControllerRemoveStateValue)
            {
                Hooks::SwapVTable(L"AFortPlayerController::ServerRemoveInventoryStateValue_Implementation", VTable, StateValueIndex, reinterpret_cast<void*>(&ServerRemoveInventoryStateValue_ImplementationHK));
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::ServerRemoveInventoryStateValue_Implementation slot not found");
    }

    for (const auto& [Function, Implementation, Name] : {
             std::tuple{ StaticFunction_ServerExecuteInventoryItem(), reinterpret_cast<void*>(&ServerExecuteInventoryItem_ImplementationHK), L"AFortPlayerController::ServerExecuteInventoryItem_Implementation" },
             std::tuple{ StaticFunction_ServerSpawnInventoryDrop(), reinterpret_cast<void*>(&ServerSpawnInventoryDrop_ImplementationHK), L"AFortPlayerController::ServerSpawnInventoryDrop_Implementation" },
             std::tuple{ StaticFunction_ServerSpotActor(), reinterpret_cast<void*>(&ServerSpotActor_ImplementationHK), L"AFortPlayerController::ServerSpotActor_Implementation" },
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

ptrdiff_t AFortPlayerController::CriticalMatchBonusOffset()
{
    return Member_bShouldReceiveCriticalMatchBonus.GetOffsetChecked();
}

bool AFortPlayerController::ResolveDurability()
{
    return Finder::ResolveDurabilityLive(StaticFunction_ServerModDurability(), StaticClass()->GetDefaultObject(false), FFortItemEntry::DurabilityOffset(),
        AFortWeapon::PendingDestroyDueToDurabilityOffset(), AFortWeapon::PendingDestroyDueToDurabilityMask());
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
    static_cast<AFortPlayerController*>(Context)->ActivateSlot(EFortQuickBars{ Z_Param_InQuickBar }, Z_Param_Slot, Z_Param_ActivateDelay, Z_Param_bUpdatePreviousFocusedSlot);
    P_NATIVE_END;
}

void AFortPlayerController::GivePlayerAmmo(int32_t AmmoCount)
{
#if WITH_SERVER_CODE
    AFortPawn* Pawn = MyFortPawn;
    AFortWeapon* Weapon = Role == ROLE_Authority && Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    if (!Weapon || AmmoCount <= 0)
    {
        return;
    }

    const int32_t BulletsPerClip = Weapon->GetBulletsPerClip();
    const int32_t Refunded = std::min(AmmoCount, BulletsPerClip - static_cast<int32_t>(Weapon->AmmoCount));
    if (Refunded > 0)
    {
        Weapon->ModAmmo(Refunded);
    }
#endif
}

DEFINE_FUNCTION(AFortPlayerController::execGivePlayerAmmo)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_AmmoCount);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->GivePlayerAmmo(Z_Param_AmmoCount);
    P_NATIVE_END;
}

void AFortPlayerController::SwapQuickBarFocus(EFortQuickBars InQuickBar)
{
#if WITH_SERVER_CODE
    AFortQuickBars* Bars = QuickBars;
    if (Bars && Bars->GetCurrentQuickBar() != InQuickBar)
    {
        Bars->SwapQuickBarFocus(InQuickBar, -1);
    }
#endif
}

DEFINE_FUNCTION(AFortPlayerController::execSwapQuickBarFocus)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_InQuickBar);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->SwapQuickBarFocus(EFortQuickBars{ Z_Param_InQuickBar });
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

bool AFortPlayerController::ModDurabilityHK(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet)
{
    AFortPlayerController* PlayerController = FromInventoryOwner(static_cast<IFortInventoryOwnerInterface*>(InventoryOwner));
    return PlayerController->ModDurability(ItemGuid, DurabilityMod, bForceSet);
}

void AFortPlayerController::PossessHK(AFortPlayerController* This, APawn* InPawn)
{
    This->Possess(InPawn);
}

void AFortPlayerController::TrySetHeroImplementationHK(AFortPlayerController* This, UFortHero* NewHero, bool bSendToServer, bool bUnused)
{
    This->TrySetHeroImplementation(NewHero, bSendToServer, bUnused);
}

void AFortPlayerController::ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid)
{
    This->ServerExecuteInventoryItem_Implementation(ItemGuid);
}

void AFortPlayerController::ServerSpotActor_ImplementationHK(AFortPlayerController* This, AActor* NewlySpottedActor)
{
    This->ServerSpotActor_Implementation(NewlySpottedActor);
}

void AFortPlayerController::ServerRemoveInventoryStateValue_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType)
{
    This->ServerRemoveInventoryStateValue_Implementation(ItemGuid, StateType);
}

void AFortPlayerController::ServerSpawnInventoryDrop_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, int32_t Count)
{
    This->ServerSpawnInventoryDrop_Implementation(ItemGuid, Count);
}

void AFortPlayerController::Hook()
{
    Hooks::Detour(L"AFortPlayerController::InitializeMcpProfile", reinterpret_cast<void*>(Natives::AFortPlayerController_InitializeMcpProfile), &InitializeMcpProfileHK, &InitializeMcpProfileOG);
    Hooks::Detour(L"AFortPlayerController::TrySetHeroImplementation", reinterpret_cast<void*>(Natives::AFortPlayerController_TrySetHeroImplementation), &TrySetHeroImplementationHK, &TrySetHeroImplementationOG);
}

bool AFortPlayerController::InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete)
{
    return This->InitializeMcpProfile(OnComplete);
}
