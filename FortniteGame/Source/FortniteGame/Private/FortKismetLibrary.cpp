#include "pch.h"
#include "FortKismetLibrary.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIGoalManager.h"
#include "Abilities/FortAbilitySet.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortQuickBars.h"
#include "Log.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
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
    if (!Inventory || !ItemDefinition || NumberToGive <= 0)
    {
        return;
    }

    const int32_t MaxStackSize = std::max(static_cast<int32_t>(ItemDefinition->MaxStackSize), 1);
    int32_t Remaining = NumberToGive;

    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num() && Remaining > 0; ++Index)
    {
        UFortWorldItem* Existing = Instances[Index];
        FFortItemEntry& ExistingEntry = Existing->ItemEntry;
        if (static_cast<UFortItemDefinition*>(ExistingEntry.ItemDefinition) == ItemDefinition && ExistingEntry.Count < MaxStackSize)
        {
            const int32_t Added = std::min(Remaining, MaxStackSize - ExistingEntry.Count);
            Inventory->UpdateWorldItemCount(Existing, ExistingEntry.Count + Added);
            Remaining -= Added;
        }
    }

    while (Remaining > 0)
    {
        const int32_t Count = std::min(Remaining, MaxStackSize);
        UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(Count, 1));
        if (!WorldItem)
        {
            return;
        }

        WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
        Inventory->AddWorldItem(WorldItem);
        PlayerController->TryAddToQuickBar(WorldItem);
        Remaining -= Count;
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

void UFortKismetLibrary::HookNatives()
{
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

    for (const auto& [Function, Native, Name] : {
             std::tuple{ StaticFunction_GetAIDirector(), &execGetAIDirector, L"UFortKismetLibrary::GetAIDirector" },
             std::tuple{ StaticFunction_GetAIGoalManager(), &execGetAIGoalManager, L"UFortKismetLibrary::GetAIGoalManager" },
             std::tuple{ StaticFunction_GiveItemToInventoryOwner(), &execGiveItemToInventoryOwner, L"UFortKismetLibrary::GiveItemToInventoryOwner" },
             std::tuple{ StaticFunction_K2_GiveItemToPlayer(), &execK2_GiveItemToPlayer, L"UFortKismetLibrary::K2_GiveItemToPlayer" },
             std::tuple{ StaticFunction_K2_RemoveItemFromPlayer(), &execK2_RemoveItemFromPlayer, L"UFortKismetLibrary::K2_RemoveItemFromPlayer" },
             std::tuple{ StaticFunction_K2_RemoveItemFromAllPlayers(), &execK2_RemoveItemFromAllPlayers, L"UFortKismetLibrary::K2_RemoveItemFromAllPlayers" } })
    {
        if (Function)
        {
            Function->SetNativeFunc(Native);
            Log::Info(L"Hook {}: native", Name);
        }
    }
}
