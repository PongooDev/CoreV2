#include "pch.h"
#include "FortKismetLibrary.h"
#include "AI/FortAIDirector.h"
#include "Athena/FortGameModeAthena.h"
#include "AI/FortAIGoalManager.h"
#include "Abilities/FortAbilitySet.h"
#include "Building/BuildingSMActor.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortLootPackage.h"
#include "Engine/DataTable.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "Items/FortPickup.h"
#include "Math/Rotator.h"
#include "Math/UnrealMathUtility.h"
#include "Missions/FortItemCollectedParams.h"
#include "Items/FortQuickBars.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortCheatManager.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <cmath>
#include <vector>
#include "ReflectionParams.h"
#include "UObject/Class.h"

void UFortKismetLibrary::EquipFortAbilitySet(TScriptInterface<IAbilitySystemInterface> AbilitySystemInterfaceActor, UFortAbilitySet* AbilitySet, UObject* OverrideSourceObject, FFortAbilitySetHandle& OutHandle)
{
    Reflection::FParams Params(StaticFunction_EquipFortAbilitySet());
    Params.Set(L"AbilitySystemInterfaceActor", static_cast<const FScriptInterface&>(AbilitySystemInterfaceActor));
    Params.Set(L"AbilitySet", AbilitySet);
    Params.Set(L"OverrideSourceObject", OverrideSourceObject);
    if (Params.Call(StaticClass()->GetDefaultObject()))
    {
        Params.Relocate(L"ReturnValue", &OutHandle, FFortAbilitySetHandle::StaticStruct()->GetStructureSize());
    }
}

AFortAIDirector* UFortKismetLibrary::GetAIDirector(UObject* WorldContextObject)
{
    return AFortAIDirector::GetCurrent(WorldContextObject);
}

AFortAIGoalManager* UFortKismetLibrary::GetAIGoalManager(UObject* WorldContextObject)
{
    return AFortAIGoalManager::GetCurrent(WorldContextObject);
}

void UFortKismetLibrary::GiveItemToInventoryOwner(TScriptInterface<IFortInventoryOwnerInterface> InventoryOwner, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer, int32_t Level, bool bLoadFullClip, bool bDropOverflow)
{
#if WITH_SERVER_CODE
    AFortPlayerController* PlayerController = Cast<AFortPlayerController>(InventoryOwner.GetObject());
    AFortInventory* Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    if (!PlayerController && InventoryOwner.GetInterface() && Natives::IFortInventoryOwnerInterface_GetWorldInventoryIndex >= 0)
    {
        Inventory = InventoryOwner->GetWorldInventory();
    }
    if (!Inventory || !ItemDefinition || NumberToGive <= 0)
    {
        return;
    }

    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* ItemEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    FFortItemEntry::Construct(ItemEntry, ItemDefinition, NumberToGive, Level);

    // The game's give keeps a backpack-limited item (a weapon, a trap) past the backpack's size, marked as overflow. A
    // cheat's (#188) gives only what fits, as a pickup does (GiveItemEntry): the overflow is measured for those items
    // too, existing stacks fill first, no new stack goes in once the backpack is full, and the rest drops below.
    const bool bBackpack = PlayerController && static_cast<AFortInventory*>(PlayerController->WorldInventory) == Inventory;
    const bool bFitOnly = bDropOverflow && bBackpack;
    const int32_t Count = NumberToGive - Inventory->GetOverflowAmount(*ItemEntry, bFitOnly);
    const int32_t Given = Inventory->AddItemStacks(*ItemEntry, Count, -1, bBackpack, !bFitOnly,
        [&](UFortWorldItem* Existing, int32_t Added)
        {
            FFortItemEntry& ExistingEntry = Existing->ItemEntry;
            Inventory->UpdateWorldItemCount(Existing, ExistingEntry.Count + Added);
            if (PlayerController && bNotifyPlayer)
            {
                ExistingEntry.AddNewItemCount(Added, true);
            }
        },
        [&](int32_t StackCount, bool bOverflow) -> UFortWorldItem*
        {
            UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(StackCount, Level));
            if (!WorldItem)
            {
                return nullptr;
            }

            FFortItemEntry& NewEntry = WorldItem->ItemEntry;
            NewEntry.inventory_overflow_date = bOverflow;
            if (bLoadFullClip)
            {
                NewEntry.LoadFullClip();
            }
            if (PlayerController)
            {
                WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
            }
            Inventory->AddWorldItem(WorldItem);
            if (PlayerController)
            {
                if (bNotifyPlayer)
                {
                    NewEntry.SetNewItemCount(StackCount, true);
                }
                if (!bOverflow)
                {
                    PlayerController->TryAddToQuickBar(WorldItem);
                }
            }
            return WorldItem;
        });

    AFortPawn* Pawn = PlayerController ? static_cast<AFortPawn*>(PlayerController->MyFortPawn) : nullptr;
    if (Given < NumberToGive && Pawn)
    {
        ItemEntry->Count = NumberToGive - Given;
        Log::Info(L"GiveItemToInventoryOwner: {} of {} {} for {} didn't fit, they drop at the pawn", static_cast<int32_t>(ItemEntry->Count), NumberToGive, ItemDefinition->GetName().ToStdString(), PlayerController->GetName().ToStdString());
        if (bLoadFullClip)
        {
            ItemEntry->LoadFullClip();
        }
        const FVector Location = Pawn->GetTransform().GetLocation();
        if (AFortPickup* Pickup = AFortPickup::CreatePickup(PlayerController->GetWorld(), *ItemEntry, &Location))
        {
            Pickup->TossPickup(Location, Pawn, 0);
        }
    }
    EntryStruct->DestroyStruct(ItemEntry);

    if (PlayerController && Given > 0)
    {
        UFortItemCollectedParams::SendItemCollected(PlayerController, ItemDefinition, Given);
    }
#endif
}

void UFortKismetLibrary::K2_GiveItemToPlayer(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer, int32_t Level, bool bLoadFullClip, bool bDropOverflow)
{
#if WITH_SERVER_CODE
    if (PlayerController && ItemDefinition)
    {
        GiveItemToInventoryOwner(TScriptInterface<IFortInventoryOwnerInterface>(PlayerController, nullptr), ItemDefinition, NumberToGive, bNotifyPlayer, Level, bLoadFullClip, bDropOverflow);
    }
#endif
}

void UFortKismetLibrary::K2_GiveItemToAllPlayers(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer)
{
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (!World || !ItemDefinition)
    {
        return;
    }

    for (TObjectIterator<AFortPlayerController> It; It; ++It)
    {
        if (It->GetWorld() == World)
        {
            K2_GiveItemToPlayer(*It, ItemDefinition, NumberToGive, bNotifyPlayer);
        }
    }
#endif
}

int32_t UFortKismetLibrary::K2_RemoveItemFromPlayer(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove, bool bForceRemoval)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    if (!Inventory || !ItemDefinition)
    {
        return 0;
    }

    std::vector<UFortWorldItem*> Matching;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry& Entry = Instances[Index]->ItemEntry;
        if (static_cast<UFortItemDefinition*>(Entry.ItemDefinition) == ItemDefinition)
        {
            Matching.push_back(Instances[Index]);
        }
    }

    int32_t Removed = 0;
    for (UFortWorldItem* Item : Matching)
    {
        if (AmountToRemove >= 0 && Removed >= AmountToRemove)
        {
            break;
        }

        FFortItemEntry& Entry = Item->ItemEntry;
        const int32_t Count = Entry.Count;
        const int32_t Taken = AmountToRemove < 0 ? Count : std::min(Count, AmountToRemove - Removed);
        const FGuid ItemGuid = Entry.ItemGuid;
        if (PlayerController->RemoveInventoryItem(ItemGuid, Taken, true, bForceRemoval))
        {
            Removed += Taken;
        }
    }
    return Removed;
#else
    return 0;
#endif
}

int32_t UFortKismetLibrary::K2_RemoveItemFromPlayerByGuid(AFortPlayerController* PlayerController, const FGuid& ItemGuid, int32_t AmountToRemove, bool bForceRemoval)
{
#if WITH_SERVER_CODE
    // 5.40's native (CL-4352937 0xEBD4D0) keeps only its check, a controller and a valid guid, around a call to the shared
    // return-0 stub, so a Blueprint's removal by guid took nothing. The Grappler's is one: GA_Athena_HookDestroy (granted
    // while it is held, by GA_Athena_HookPassive's GE_Athena_HookPassive, and triggered by each shot's Event.Weapons.Fire)
    // finds the magazine at 0 at the grapple travel montage's notify, plays the gun toss and, at its notify, removes the
    // gun with K2_RemoveItemFromPlayerByGuid(its controller, the weapon's GetInventoryGUID, 1, true). The spent Grappler
    // stayed in the inventory at 0 (#391).
    AFortInventory* Inventory = PlayerController && ItemGuid.IsValid() ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem || !AFortPlayerController::HasInventoryRemovalHook())
    {
        return 0;
    }

    // As K2_RemoveItemFromPlayer takes from each stack: the stack's count at most, all of it for a negative amount.
    const int32_t Count = static_cast<FFortItemEntry&>(WorldItem->ItemEntry).Count;
    const int32_t Taken = AmountToRemove < 0 ? Count : std::min(Count, AmountToRemove);
    return PlayerController->RemoveInventoryItem(ItemGuid, Taken, true, bForceRemoval) ? Taken : 0;
#else
    return 0;
#endif
}

uint8_t UFortKismetLibrary::GetActorTeam(const AActor* Actor)
{
    UFunction* Function = FindStaticFunction_GetActorTeam();
    UObject* LibraryDefault = Function ? StaticClass()->GetDefaultObject(false) : nullptr;
    Reflection::FParams Params(Function);
    if (!LibraryDefault || !Params.Set(L"Actor", Actor) || !Params.Call(LibraryDefault))
    {
        return Reflection::InvalidEnumValue;
    }
    return Params.Take<uint8_t>(L"ReturnValue");
}

bool UFortKismetLibrary::HasForceRemoval()
{
    static const bool bHasForceRemoval = StaticFunction_K2_RemoveItemFromPlayer()->FindPropertyByName(FName(L"bForceRemoval", FNAME_Find)) != nullptr;
    return bHasForceRemoval;
}

int32_t UFortKismetLibrary::K2_RemoveItemFromPlayerHK(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove, bool bForceRemoval)
{
    return K2_RemoveItemFromPlayer(PlayerController, ItemDefinition, AmountToRemove, HasForceRemoval() && bForceRemoval);
}

void UFortKismetLibrary::GiveItemToInventoryOwnerHK(TScriptInterface<IFortInventoryOwnerInterface> InventoryOwner, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer)
{
    GiveItemToInventoryOwner(InventoryOwner, ItemDefinition, NumberToGive, bNotifyPlayer);
}

void UFortKismetLibrary::K2_RemoveItemFromAllPlayers(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove)
{
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (!World || !ItemDefinition)
    {
        return;
    }

    for (TObjectIterator<AFortPlayerController> It; It; ++It)
    {
        if (It->GetWorld() == World)
        {
            K2_RemoveItemFromPlayer(*It, ItemDefinition, AmountToRemove);
        }
    }
#endif
}

UFortWorldItemDefinition* UFortKismetLibrary::K2_GetResourceItemDefinition(uint8_t ResourceType)
{
    // The game data's item of each resource type, as a harvested piece gives (ABuildingSMActor::OnDamageServer).
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (!GameData)
    {
        return nullptr;
    }
    if (ResourceType == EFortResourceType::GetWood())
    {
        return GameData->WoodItemDefinition;
    }
    if (ResourceType == EFortResourceType::GetStone())
    {
        return GameData->StoneItemDefinition;
    }
    if (ResourceType == EFortResourceType::GetMetal())
    {
        return GameData->MetalItemDefinition;
    }
    if (ResourceType == EFortResourceType::GetPermanite())
    {
        return GameData->PermaniteItemDefinition;
    }
    return nullptr;
}

void UFortKismetLibrary::K2_GiveBuildingResource(AFortPlayerController* Controller, uint8_t ResourceType, int32_t ResourceAmount)
{
#if WITH_SERVER_CODE
    // The client builds strip its body: 6.21's exec (0x1553060) steps its three parameters and calls the shared empty
    // function. 6.20's and 6.21's Playground building challenges (MinigameMode_BuildTheStructure: Build The Box, Ramp,
    // Tower and Llama) call it from their K2_PreGameTransitionEvent's GiveWood: K2_RemoveItemFromPlayer takes all of the
    // player's wood, then this gives AmmountOfWood (500) of it back. Stripped, starting a building challenge only took
    // the player's wood (CoreV2 #438). 🟡 The body is inferred from its parameters: the resource type's item, given as a
    // harvest gives it.
    UFortWorldItemDefinition* ResourceItem = K2_GetResourceItemDefinition(ResourceType);
    if (!Controller || !ResourceItem || ResourceAmount <= 0)
    {
        return;
    }
    K2_GiveItemToPlayer(Controller, ResourceItem, ResourceAmount, false);
#endif
}

AFortPickup* UFortKismetLibrary::K2_SpawnPickupInWorld(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, const FVector& Position, const FVector& Direction, int32_t OverrideMaxStackCount, bool bToss, bool bRandomRotation, bool bBlockedFromAutoPickup)
{
    // bBlockedFromAutoPickup (5.30): 5.30's native is cut (the exec calls a return-0 stub) and its AFortPickup has no
    // member the flag lands on, so it's taken for the call's sake and left unused.
    (void)bBlockedFromAutoPickup;
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    if (!World || !ItemDefinition || NumberToSpawn <= 0)
    {
        return nullptr;
    }

    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* ItemEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    FFortItemEntry::Construct(ItemEntry, ItemDefinition, NumberToSpawn, 0);
    // Battle Royale's supply drop (AthenaSupplyDrop_02) spawns its loot through here: its weapons come loaded (#85).
    if (AFortGameModeAthena::GetCurrent(World))
    {
        ItemEntry->LoadFullClip();
    }

    const FRotator SpawnRotation = Direction.Rotation();
    AFortPickup* Pickup = AFortPickup::CreatePickup(World, *ItemEntry, &Position, &SpawnRotation);
    EntryStruct->DestroyStruct(ItemEntry);

    // 3.0 replicates the choice on the pickup (AFortPickup::bRandomRotation).
    if (Pickup && bRandomRotation && AFortPickup::HasRandomRotation())
    {
        Pickup->bRandomRotation = true;
    }

    // Before 1.8 every pickup spawned here is tossed; 1.8 lets the caller choose (bToss).
    if (Pickup && bToss)
    {
        Pickup->TossPickup(Position, nullptr, OverrideMaxStackCount);
    }
    return Pickup;
#else
    return nullptr;
#endif
}

bool UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(FName& OutLootTierKey, int32_t& OutLootTier, FName TierGroupName, int32_t WorldLevel, int32_t ForcedLootTier, const FGameplayTagContainer& ContextTags)
{
    std::vector<FFortLootTierRow> Rows;
    float TotalWeight = 0.0f;
    UFortLootTier::GetLootTierRows(Rows, TotalWeight, TierGroupName, WorldLevel, -1, ContextTags, ELootQuotaLevel::GetUnlimited());

    if (ForcedLootTier != -1)
    {
        struct FTierGroup
        {
            int32_t LootTier;
            std::vector<FFortLootTierRow> Rows;
            float Weight;
        };
        std::vector<FTierGroup> Groups;
        for (const FFortLootTierRow& Row : Rows)
        {
            const int32_t LootTier = Row.LootTierData->LootTier;
            auto Group = std::find_if(Groups.begin(), Groups.end(), [LootTier](const FTierGroup& Existing) { return Existing.LootTier == LootTier; });
            if (Group == Groups.end())
            {
                Groups.push_back({ LootTier, { Row }, Row.LootTierData->Weight });
            }
            else
            {
                Group->Rows.push_back(Row);
                Group->Weight += Row.LootTierData->Weight;
            }
        }

        Rows.clear();
        TotalWeight = 0.0f;
        int32_t LootTier = ForcedLootTier;
        do
        {
            auto Group = std::find_if(Groups.begin(), Groups.end(), [LootTier](const FTierGroup& Existing) { return Existing.LootTier == LootTier; });
            if (Group != Groups.end() && !Group->Rows.empty())
            {
                Rows = Group->Rows;
                TotalWeight = Group->Weight;
            }
            --LootTier;
        } while (LootTier >= 0 && Rows.empty());
    }

    return UFortLootTier::PickLootTierFromRows(OutLootTierKey, OutLootTier, TotalWeight, Rows, nullptr);
}

bool UFortKismetLibrary::PickLootDrops(TStructArray<FFortItemEntry>& OutLootToDrop, FName TierGroupName, int32_t WorldLevel, int32_t ForcedLootTier)
{
#if WITH_SERVER_CODE
    FName LootTierKey;
    int32_t LootTier = -1;
    const FGameplayTagContainer ContextTags;
    if (!PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, LootTier, TierGroupName, WorldLevel, ForcedLootTier, ContextTags))
    {
        return false;
    }
    return UFortLootPackage::PickLootDrops(OutLootToDrop, WorldLevel, LootTierKey, nullptr, ContextTags, false, false);
#else
    return false;
#endif
}

bool UFortKismetLibrary::PickLootDrops(UObject* WorldContextObject, TStructArray<FFortItemEntry>& OutLootToDrop, FName TierGroupName, int32_t WorldLevel, int32_t ForcedLootTier)
{
    return PickLootDrops(OutLootToDrop, TierGroupName, WorldLevel, ForcedLootTier);
}

DEFINE_FUNCTION(UFortKismetLibrary::execGetAIDirector)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortAIDirector**>(RESULT_PARAM) = GetAIDirector(Z_Param_WorldContextObject);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execGetAIGoalManager)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortAIGoalManager**>(RESULT_PARAM) = GetAIGoalManager(Z_Param_WorldContextObject);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execGiveItemToInventoryOwner)
{
    P_GET_TINTERFACE(IFortInventoryOwnerInterface, Z_Param_InventoryOwner);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToGive);
    P_GET_UBOOL(Z_Param_bNotifyPlayer);
    P_FINISH;
    P_NATIVE_BEGIN;
    GiveItemToInventoryOwner(Z_Param_InventoryOwner, Z_Param_ItemDefinition, Z_Param_NumberToGive, Z_Param_bNotifyPlayer);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execK2_GiveItemToPlayer)
{
    P_GET_OBJECT(AFortPlayerController, Z_Param_PlayerController);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToGive);
    P_GET_UBOOL(Z_Param_bNotifyPlayer);
    P_FINISH;
    P_NATIVE_BEGIN;
    K2_GiveItemToPlayer(Z_Param_PlayerController, Z_Param_ItemDefinition, Z_Param_NumberToGive, Z_Param_bNotifyPlayer);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execK2_GiveItemToAllPlayers)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToGive);
    P_GET_UBOOL(Z_Param_bNotifyPlayer);
    P_FINISH;
    P_NATIVE_BEGIN;
    K2_GiveItemToAllPlayers(Z_Param_WorldContextObject, Z_Param_ItemDefinition, Z_Param_NumberToGive, Z_Param_bNotifyPlayer);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execK2_RemoveItemFromPlayer)
{
    // 3.2 added bForceRemoval (3.3's exec, 0x10C6710, reads four parameters). Reading three left the call's bytecode on
    // it, so P_FINISH stepped over its EX_True and the script loop spun on EX_EndFunctionParms, as K2_SpawnPickupInWorld's
    // did (#250): 3.3's Remote Explosives remove the emptied detonator with it true (GA_Athena_C4_Passive once the last
    // thrown charge has exploded, GAT_Athena_c4_Detonate after detonating with none left), and shooting a charge set
    // that off once the last one was thrown, which froze the server's game thread.
    P_GET_OBJECT(AFortPlayerController, Z_Param_PlayerController);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_AmountToRemove);
    bool Z_Param_bForceRemoval = false;
    if (HasForceRemoval())
    {
        P_GET_UBOOL(Z_Param_bForceRemovalParam);
        Z_Param_bForceRemoval = Z_Param_bForceRemovalParam;
    }
    P_FINISH;
    P_NATIVE_BEGIN;
#if WITH_SERVER_CODE
    // A Blueprint spending a consumable it used: "cheat infiniteconsumables" (#155) keeps it, and the use counts as paid.
    // So does a gadget's charge for "cheat nocooldown" (#148).
    const bool bKeep = Z_Param_AmountToRemove > 0
        && (UFortCheatManager::KeepsConsumable(Z_Param_PlayerController, Z_Param_ItemDefinition) || UFortCheatManager::KeepsAbilityCharge(Z_Param_PlayerController, Z_Param_ItemDefinition));
#else
    const bool bKeep = false;
#endif
    *static_cast<int32_t*>(RESULT_PARAM) = bKeep ? Z_Param_AmountToRemove : K2_RemoveItemFromPlayer(Z_Param_PlayerController, Z_Param_ItemDefinition, Z_Param_AmountToRemove, Z_Param_bForceRemoval);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execK2_RemoveItemFromPlayerByGuid)
{
    // 5.40's exec (0x145A840) reads the four parameters and calls the native.
    P_GET_OBJECT(AFortPlayerController, Z_Param_PlayerController);
    P_GET_STRUCT(FGuid, Z_Param_ItemGuid);
    P_GET_PROPERTY(UIntProperty, Z_Param_AmountToRemove);
    P_GET_UBOOL(Z_Param_bForceRemoval);
    P_FINISH;
    P_NATIVE_BEGIN;
#if WITH_SERVER_CODE
    // As K2_RemoveItemFromPlayer's: a consumable a Blueprint spends stays for "cheat infiniteconsumables" (#155), and a
    // gadget's charge for "cheat nocooldown" (#148), the use counting as paid.
    const bool bKeep = Z_Param_PlayerController && Z_Param_AmountToRemove > 0
        && (UFortCheatManager::KeepsConsumable(Z_Param_PlayerController, Z_Param_ItemGuid) || UFortCheatManager::KeepsAbilityCharge(Z_Param_PlayerController, Z_Param_ItemGuid));
#else
    const bool bKeep = false;
#endif
    *static_cast<int32_t*>(RESULT_PARAM) = bKeep ? Z_Param_AmountToRemove : K2_RemoveItemFromPlayerByGuid(Z_Param_PlayerController, Z_Param_ItemGuid, Z_Param_AmountToRemove, Z_Param_bForceRemoval);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execK2_RemoveItemFromAllPlayers)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_AmountToRemove);
    P_FINISH;
    P_NATIVE_BEGIN;
    K2_RemoveItemFromAllPlayers(Z_Param_WorldContextObject, Z_Param_ItemDefinition, Z_Param_AmountToRemove);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execK2_SpawnPickupInWorld)
{
    // 1.8 added bToss (its exec, 0xB16580, reads seven parameters); 1.1 to 1.7.2 have six. Reading six left a 1.8 call's
    // bytecode on its last parameter: P_FINISH stepped over bToss's EX_True instead of EX_EndFunctionParms, and the
    // script loop then ran EX_EndFunctionParms as a statement, whose handler (0x1432C00) steps the code back onto itself.
    // The server's game thread spun there for good: the Save the World supply drop's SpawnTierDrops calls this with bToss
    // true when it is searched. 3.0 added bRandomRotation after it (its exec, 0x1046D70, reads eight), and reading seven
    // spun 3.0's game thread the same way when a quest prop dropped its quest item (#232: the harvesting quests'
    // InternalSpawnQuestItem calls this with bToss and bRandomRotation true). 5.30 added bBlockedFromAutoPickup after
    // that (CL-4305896's exec, 0x1443EB0, reads nine), and reading eight spun 5.30's game thread again when the Save the
    // World supply drop gadget's crate (B_SupplyDropPlacement_C) was searched: its SpawnTierDrops passes it false.
    static const bool bHasToss = StaticFunction_K2_SpawnPickupInWorld()->FindPropertyByName(FName(L"bToss", FNAME_Find)) != nullptr;
    static const bool bHasRandomRotation = StaticFunction_K2_SpawnPickupInWorld()->FindPropertyByName(FName(L"bRandomRotation", FNAME_Find)) != nullptr;
    static const bool bHasBlockedFromAutoPickup = StaticFunction_K2_SpawnPickupInWorld()->FindPropertyByName(FName(L"bBlockedFromAutoPickup", FNAME_Find)) != nullptr;
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToSpawn);
    P_GET_STRUCT(FVector, Z_Param_Position);
    P_GET_STRUCT(FVector, Z_Param_Direction);
    P_GET_PROPERTY(UIntProperty, Z_Param_OverrideMaxStackCount);
    bool Z_Param_bToss = true;
    if (bHasToss)
    {
        P_GET_UBOOL(Z_Param_bTossParam);
        Z_Param_bToss = Z_Param_bTossParam;
    }
    bool Z_Param_bRandomRotation = false;
    if (bHasRandomRotation)
    {
        P_GET_UBOOL(Z_Param_bRandomRotationParam);
        Z_Param_bRandomRotation = Z_Param_bRandomRotationParam;
    }
    bool Z_Param_bBlockedFromAutoPickup = false;
    if (bHasBlockedFromAutoPickup)
    {
        P_GET_UBOOL(Z_Param_bBlockedFromAutoPickupParam);
        Z_Param_bBlockedFromAutoPickup = Z_Param_bBlockedFromAutoPickupParam;
    }
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortPickup**>(RESULT_PARAM) = K2_SpawnPickupInWorld(Z_Param_WorldContextObject, Z_Param_ItemDefinition, Z_Param_NumberToSpawn, Z_Param_Position, Z_Param_Direction, Z_Param_OverrideMaxStackCount, Z_Param_bToss, Z_Param_bRandomRotation, Z_Param_bBlockedFromAutoPickup);
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortKismetLibrary::execK2_GiveBuildingResource)
{
    // 6.21's exec (0x1553060): the controller, the EFortResourceType byte and the amount.
    P_GET_OBJECT(AFortPlayerController, Z_Param_Controller);
    P_GET_PROPERTY(UByteProperty, Z_Param_ResourceType);
    P_GET_PROPERTY(UIntProperty, Z_Param_ResourceAmount);
    P_FINISH;
    P_NATIVE_BEGIN;
    K2_GiveBuildingResource(Z_Param_Controller, Z_Param_ResourceType, Z_Param_ResourceAmount);
    P_NATIVE_END;
}

void UFortKismetLibrary::HookNatives()
{
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

#if WITH_SERVER_CODE
    // 1.2 cut the loot roll: the exec calls a shared return-false stub instead of the native. 5.0's native takes the
    // WorldContextObject first (CL-4204761's exec calls it at 0x13B9876 with rcx = WorldContextObject, rdx = OutLootToDrop,
    // r8 = TierGroupName, r9d = WorldLevel and ForcedLootTier on the stack). Redirected to the four-parameter function,
    // every parameter was the one before it: the tier group was OutLootToDrop's address, no tier matched, and every
    // supply drop and Supply Llama opened onto nothing ("Failed to spawn tier 0 loot from Loot Crate.").
    if (uint8_t* PickLootDropsCall = Finder::ImplementationCall(StaticFunction_PickLootDrops()); PickLootDropsCall && Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(PickLootDropsCall), 1, 5)))
    {
        const bool bHasWorldContext = StaticFunction_PickLootDrops()->FindPropertyByName(FName(L"WorldContextObject", FNAME_Find)) != nullptr;
        using FPickLootDrops = bool (*)(TStructArray<FFortItemEntry>&, FName, int32_t, int32_t);
        using FPickLootDropsWithWorldContext = bool (*)(UObject*, TStructArray<FFortItemEntry>&, FName, int32_t, int32_t);
        void* const Target = bHasWorldContext ? reinterpret_cast<void*>(static_cast<FPickLootDropsWithWorldContext>(&PickLootDrops)) : reinterpret_cast<void*>(static_cast<FPickLootDrops>(&PickLootDrops));
        Hooks::RedirectCall(bHasWorldContext ? L"UFortKismetLibrary::PickLootDrops (WorldContextObject)" : L"UFortKismetLibrary::PickLootDrops", PickLootDropsCall, Target);

        // Load (or, without them in the pak, create) the loot tables now, not from inside a level's PostLoad.
        if (UFortGameData* GameData = UFortAssetManager::Get().GetGameData())
        {
            GameData->GetLootTierDataTables();
        }
    }

    // ABuildingItemCollectorActor's deposit calls the folded native directly instead of the UFunction.
    UFunction* RemoveItemFromPlayer = StaticFunction_K2_RemoveItemFromPlayer();
    const uintptr_t RemoveItemFromPlayerExec = RemoveItemFromPlayer ? reinterpret_cast<uintptr_t>(RemoveItemFromPlayer->GetNativeFunc()) : 0;
    const uintptr_t RemoveItemFromPlayerExecEnd = RemoveItemFromPlayerExec ? Finder::FunctionEnd(RemoveItemFromPlayerExec) : 0;
    const std::vector<std::pair<uintptr_t, uintptr_t>> RemoveItemFromPlayerExecCalls = RemoveItemFromPlayerExecEnd > RemoveItemFromPlayerExec ? Finder::DirectCalls(RemoveItemFromPlayerExec, RemoveItemFromPlayerExecEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>();
    const uintptr_t RemoveItemFromPlayerNative = RemoveItemFromPlayerExecCalls.empty() ? 0 : RemoveItemFromPlayerExecCalls.back().second;
    if (!RemoveItemFromPlayerNative || RemoveItemFromPlayerNative == reinterpret_cast<uintptr_t>(Natives::FFrame_Step) || RemoveItemFromPlayerNative == reinterpret_cast<uintptr_t>(Natives::FFrame_StepExplicitProperty))
    {
        Log::Error(L"Finder: UFortKismetLibrary::K2_RemoveItemFromPlayer native not found");
    }
    else
    {
        for (const uintptr_t Site : Finder::CallsTo(RemoveItemFromPlayerNative, false))
        {
            Hooks::RedirectCall(L"UFortKismetLibrary::K2_RemoveItemFromPlayer", reinterpret_cast<uint8_t*>(Site), reinterpret_cast<void*>(&K2_RemoveItemFromPlayerHK));
        }
    }

    // 6.00's Shadow Stones (#400). A stone's GA_Consume_SpookyMist applies GE_CBGA_SpookyMist at its montage's notify and
    // calls the controller's AFortPlayerControllerGameplay::StartGhostMode(AGID_SpookyMist), which gives that gadget with
    // GiveItemToInventoryOwner before it sets GhostModeRepData. That call reaches the stripped native, so nothing was
    // given: no shadow form ability set (GA_SpookyMist_PassiveSetup, GA_SpookyMist_Travel's Phase, the speed and
    // fall-damage effects), no shadow body, and the client's OnRep_GhostModeRepData waited for an item that never came
    // (CheckGhostModeItemReplicated). EndGhostMode's removal goes through the K2_RemoveItemFromPlayer native above.
    if (uint8_t* GiveItemCall = Finder::StartGhostModeGiveItemCall())
    {
        Hooks::RedirectCall(L"AFortPlayerControllerGameplay::StartGhostMode -> UFortKismetLibrary::GiveItemToInventoryOwner", GiveItemCall, reinterpret_cast<void*>(&GiveItemToInventoryOwnerHK));
    }

    // 6.01's Disco Domination. A player who dies while the mode still respawns its players leaves a share of each ammo
    // stack where they fell (Disco.AmmoDropPercent), and keeps the rest with everything else they held: the Disco
    // mutator's AFortGameModeAthena_OnPawnDeathBeforeRespawning spawns those pickups with K2_SpawnPickupInWorld, a call
    // that reaches the stripped native, so nothing dropped. It passes the native's nine parameters, the ones
    // K2_SpawnPickupInWorld takes here. The exec below still has the game's own function at this point: the finder reads
    // the stub from it.
    if (uint8_t* SpawnPickupCall = Finder::DiscoPawnDeathSpawnPickupCall(StaticFunction_K2_SpawnPickupInWorld()))
    {
        using FSpawnPickupInWorld = AFortPickup* (*)(UObject*, UFortWorldItemDefinition*, int32_t, const FVector&, const FVector&, int32_t, bool, bool, bool);
        Hooks::RedirectCall(L"AFortAthenaMutator_Disco::AFortGameModeAthena_OnPawnDeathBeforeRespawning -> UFortKismetLibrary::K2_SpawnPickupInWorld", SpawnPickupCall,
            reinterpret_cast<void*>(static_cast<FSpawnPickupInWorld>(&K2_SpawnPickupInWorld)));
    }
#endif

    for (const auto& [Function, Native, Name] : {
             std::tuple{ StaticFunction_GetAIDirector(), &execGetAIDirector, L"UFortKismetLibrary::GetAIDirector" },
             std::tuple{ StaticFunction_GetAIGoalManager(), &execGetAIGoalManager, L"UFortKismetLibrary::GetAIGoalManager" },
             std::tuple{ StaticFunction_GiveItemToInventoryOwner(), &execGiveItemToInventoryOwner, L"UFortKismetLibrary::GiveItemToInventoryOwner" },
             std::tuple{ StaticFunction_K2_GiveItemToPlayer(), &execK2_GiveItemToPlayer, L"UFortKismetLibrary::K2_GiveItemToPlayer" },
             std::tuple{ StaticFunction_K2_GiveItemToAllPlayers(), &execK2_GiveItemToAllPlayers, L"UFortKismetLibrary::K2_GiveItemToAllPlayers" },
             std::tuple{ StaticFunction_K2_RemoveItemFromPlayer(), &execK2_RemoveItemFromPlayer, L"UFortKismetLibrary::K2_RemoveItemFromPlayer" },
             // From 5.40 (CL-4352937) only; older builds have no such UFunction.
             std::tuple{ FindStaticFunction_K2_RemoveItemFromPlayerByGuid(), &execK2_RemoveItemFromPlayerByGuid, L"UFortKismetLibrary::K2_RemoveItemFromPlayerByGuid" },
             std::tuple{ StaticFunction_K2_RemoveItemFromAllPlayers(), &execK2_RemoveItemFromAllPlayers, L"UFortKismetLibrary::K2_RemoveItemFromAllPlayers" },
             std::tuple{ StaticFunction_K2_SpawnPickupInWorld(), &execK2_SpawnPickupInWorld, L"UFortKismetLibrary::K2_SpawnPickupInWorld" } })
    {
        if (Function)
        {
            Function->SetNativeFunc(Native);
            Log::Info(L"Hook {}: native", Name);
        }
    }

#if WITH_SERVER_CODE
    // Only where the build's exec calls a stub (6.21's calls the shared empty function); a build with the body keeps it.
    if (UFunction* GiveBuildingResource = FindStaticFunction_K2_GiveBuildingResource())
    {
        uint8_t* NativeCall = Finder::ImplementationCall(GiveBuildingResource);
        if (NativeCall && Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
        {
            GiveBuildingResource->SetNativeFunc(&execK2_GiveBuildingResource);
            Log::Info(L"Hook UFortKismetLibrary::K2_GiveBuildingResource: native");
        }
        else
        {
            Log::Info(L"Finder: UFortKismetLibrary::K2_GiveBuildingResource is native in this build");
        }
    }
#endif
}
