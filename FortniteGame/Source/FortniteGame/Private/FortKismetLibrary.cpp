#include "pch.h"
#include "FortKismetLibrary.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIGoalManager.h"
#include "Abilities/FortAbilitySet.h"
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

void UFortKismetLibrary::GiveItemToInventoryOwner(TScriptInterface<IFortInventoryOwnerInterface> InventoryOwner, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer)
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
    FFortItemEntry::Construct(ItemEntry, ItemDefinition, NumberToGive, 1);

    const bool bBackpack = PlayerController && static_cast<AFortInventory*>(PlayerController->WorldInventory) == Inventory;
    const int32_t Count = NumberToGive - Inventory->GetOverflowAmount(*ItemEntry, false);
    const int32_t Given = Inventory->AddItemStacks(*ItemEntry, Count, -1, bBackpack, true,
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
            UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(StackCount, 1));
            if (!WorldItem)
            {
                return nullptr;
            }

            FFortItemEntry& NewEntry = WorldItem->ItemEntry;
            NewEntry.inventory_overflow_date = bOverflow;
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

void UFortKismetLibrary::K2_GiveItemToPlayer(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToGive, bool bNotifyPlayer)
{
#if WITH_SERVER_CODE
    if (PlayerController && ItemDefinition)
    {
        GiveItemToInventoryOwner(TScriptInterface<IFortInventoryOwnerInterface>(PlayerController, nullptr), ItemDefinition, NumberToGive, bNotifyPlayer);
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

int32_t UFortKismetLibrary::K2_RemoveItemFromPlayer(AFortPlayerController* PlayerController, UFortWorldItemDefinition* ItemDefinition, int32_t AmountToRemove)
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
        if (PlayerController->RemoveInventoryItem(ItemGuid, Taken, true))
        {
            Removed += Taken;
        }
    }
    return Removed;
#else
    return 0;
#endif
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

AFortPickup* UFortKismetLibrary::K2_SpawnPickupInWorld(UObject* WorldContextObject, UFortWorldItemDefinition* ItemDefinition, int32_t NumberToSpawn, const FVector& Position, const FVector& Direction, int32_t OverrideMaxStackCount)
{
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

    const FRotator SpawnRotation = Direction.Rotation();
    AFortPickup* Pickup = AFortPickup::CreatePickup(World, *ItemEntry, &Position, &SpawnRotation);
    EntryStruct->DestroyStruct(ItemEntry);

    if (Pickup)
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
    P_GET_OBJECT(AFortPlayerController, Z_Param_PlayerController);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_AmountToRemove);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<int32_t*>(RESULT_PARAM) = K2_RemoveItemFromPlayer(Z_Param_PlayerController, Z_Param_ItemDefinition, Z_Param_AmountToRemove);
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
    P_GET_OBJECT(UObject, Z_Param_WorldContextObject);
    P_GET_OBJECT(UFortWorldItemDefinition, Z_Param_ItemDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_NumberToSpawn);
    P_GET_STRUCT(FVector, Z_Param_Position);
    P_GET_STRUCT(FVector, Z_Param_Direction);
    P_GET_PROPERTY(UIntProperty, Z_Param_OverrideMaxStackCount);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<AFortPickup**>(RESULT_PARAM) = K2_SpawnPickupInWorld(Z_Param_WorldContextObject, Z_Param_ItemDefinition, Z_Param_NumberToSpawn, Z_Param_Position, Z_Param_Direction, Z_Param_OverrideMaxStackCount);
    P_NATIVE_END;
}

void UFortKismetLibrary::HookNatives()
{
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

#if WITH_SERVER_CODE
    // 1.2 cut the loot roll: the exec calls a shared return-false stub instead of the native.
    if (uint8_t* PickLootDropsCall = Finder::ImplementationCall(StaticFunction_PickLootDrops()); PickLootDropsCall && Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(PickLootDropsCall), 1, 5)))
    {
        Hooks::RedirectCall(L"UFortKismetLibrary::PickLootDrops", PickLootDropsCall, reinterpret_cast<void*>(&PickLootDrops));

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
            Hooks::RedirectCall(L"UFortKismetLibrary::K2_RemoveItemFromPlayer", reinterpret_cast<uint8_t*>(Site), reinterpret_cast<void*>(&K2_RemoveItemFromPlayer));
        }
    }
#endif

    for (const auto& [Function, Native, Name] : {
             std::tuple{ StaticFunction_GetAIDirector(), &execGetAIDirector, L"UFortKismetLibrary::GetAIDirector" },
             std::tuple{ StaticFunction_GetAIGoalManager(), &execGetAIGoalManager, L"UFortKismetLibrary::GetAIGoalManager" },
             std::tuple{ StaticFunction_GiveItemToInventoryOwner(), &execGiveItemToInventoryOwner, L"UFortKismetLibrary::GiveItemToInventoryOwner" },
             std::tuple{ StaticFunction_K2_GiveItemToPlayer(), &execK2_GiveItemToPlayer, L"UFortKismetLibrary::K2_GiveItemToPlayer" },
             std::tuple{ StaticFunction_K2_GiveItemToAllPlayers(), &execK2_GiveItemToAllPlayers, L"UFortKismetLibrary::K2_GiveItemToAllPlayers" },
             std::tuple{ StaticFunction_K2_RemoveItemFromPlayer(), &execK2_RemoveItemFromPlayer, L"UFortKismetLibrary::K2_RemoveItemFromPlayer" },
             std::tuple{ StaticFunction_K2_RemoveItemFromAllPlayers(), &execK2_RemoveItemFromAllPlayers, L"UFortKismetLibrary::K2_RemoveItemFromAllPlayers" },
             std::tuple{ StaticFunction_K2_SpawnPickupInWorld(), &execK2_SpawnPickupInWorld, L"UFortKismetLibrary::K2_SpawnPickupInWorld" } })
    {
        if (Function)
        {
            Function->SetNativeFunc(Native);
            Log::Info(L"Hook {}: native", Name);
        }
    }
}
