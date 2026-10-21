#include "pch.h"
#include "Items/FortSchematicItem.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortSchematicItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

void UFortSchematicItem::GetSchematicRequirements(TStructArray<FSchematicRequirement>& Requirements) const
{
    Reflection::FParams Params(StaticFunction_GetSchematicRequirements());
    if (Params.Call(const_cast<UFortSchematicItem*>(this)))
    {
        Params.Relocate(L"Requirements", &Requirements, sizeof(FScriptArray));
    }
}

int32_t UFortSchematicItem::GetSchematicQuantityProduced() const
{
    Reflection::FParams Params(StaticFunction_GetSchematicQuantityProduced());
    return Params.Call(const_cast<UFortSchematicItem*>(this)) ? Params.Take<int32_t>(L"ReturnValue") : 0;
}

bool UFortSchematicItem::CanCraftSchematic(AFortPlayerController* PlayerController, uint8_t& OutFailCause, bool bConsiderReservedItems, int32_t Count)
{
    if (!Natives::UFortSchematicItem_CanCraftSchematic && !Finder::ResolveCraftingLive())
    {
        return false;
    }
    return Natives::UFortSchematicItem_CanCraftSchematic(this, PlayerController, &OutFailCause, bConsiderReservedItems, Count);
}

int32_t UFortSchematicItem::ServerCraftSchematic(AFortPlayerController* PlayerController, int32_t Count)
{
#if WITH_SERVER_CODE
    uint8_t FailCause = 0;
    Count = Count > 0 ? Count : 1;
    if (!PlayerController || !CanCraftSchematic(PlayerController, FailCause, false, Count))
    {
        UE_LOG(LogFort, Warning, TEXT("ServerCraftSchematic: cannot craft %s x%d (fail cause %d)"), *GetName(), Count, static_cast<int32_t>(FailCause));
        return 0;
    }

    UFortSchematicItemDefinition* Definition = Cast<UFortSchematicItemDefinition>(reinterpret_cast<UObject*>(GetItemDefinitionBP()));
    UFortWorldItemDefinition* ResultDefinition = Definition ? Definition->GetResultWorldItemDefinition() : nullptr;
    const int32_t QuantityProduced = GetSchematicQuantityProduced() * Count;
    if (!ResultDefinition || QuantityProduced <= 0)
    {
        UE_LOG(LogFort, Warning, TEXT("ServerCraftSchematic: %s has no result item (definition %s, quantity %d)"), *GetName(), *GetNameSafe(Definition), QuantityProduced);
        return 0;
    }

    if (!PlayerController->bCraftFree)
    {
        TStructArray<FSchematicRequirement> Requirements;
        GetSchematicRequirements(Requirements);
        for (int32_t Index = 0; Index < Requirements.Num(); ++Index)
        {
            FSchematicRequirement& Requirement = Requirements[Index];
            UFortWorldItemDefinition* IngredientDefinition = Cast<UFortWorldItemDefinition>(reinterpret_cast<UObject*>(static_cast<UFortItemDefinition*>(Requirement.ItemDefinition)));
            const int32_t Needed = Requirement.Count * Count;
            if (!IngredientDefinition || Needed <= 0)
            {
                continue;
            }

            const int32_t Owned = PlayerController->GetItemQuantity(IngredientDefinition);
            if (Owned < Needed)
            {
                if (UFortSchematicItem* IngredientSchematic = PlayerController->GetSchematicForItem(IngredientDefinition))
                {
                    const int32_t IngredientProduced = std::max(IngredientSchematic->GetSchematicQuantityProduced(), 1);
                    IngredientSchematic->ServerCraftSchematic(PlayerController, (Needed - Owned + IngredientProduced - 1) / IngredientProduced);
                }
            }

            if (Requirement.bIsConsumed)
            {
                UFortKismetLibrary::K2_RemoveItemFromPlayer(PlayerController, IngredientDefinition, Needed);
            }
        }
        FMemory::Free(Requirements.GetData());
    }

    if (ResultDefinition->MaxStackSize > 1)
    {
        UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, ResultDefinition, QuantityProduced, true);
        return QuantityProduced;
    }

    AFortInventory* Inventory = PlayerController->WorldInventory;
    const int32_t Level = GetLevel();
    for (int32_t Crafted = 0; Crafted < QuantityProduced; ++Crafted)
    {
        UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ResultDefinition->CreateTemporaryItemInstanceBP(1, Level));
        if (!Inventory || !WorldItem)
        {
            return Crafted;
        }

        WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
        Inventory->AddWorldItem(WorldItem);
        PlayerController->TryAddToQuickBar(WorldItem);
    }
    return QuantityProduced;
#else
    return 0;
#endif
}

void UFortSchematicItem::HookVTables()
{
    if (!Finder::ResolveCraftingLive())
    {
        return;
    }

    const int32_t Index = Natives::UFortSchematicItem_ServerCraftSchematicIndex;
    UObject* SchematicDefault = StaticClass()->GetDefaultObject(false);
    void** SchematicVTable = SchematicDefault ? *reinterpret_cast<void***>(SchematicDefault) : nullptr;
    void* const Stripped = SchematicVTable ? SchematicVTable[Index] : nullptr;
    if (!Stripped)
    {
        Log::Error(L"Finder: UFortSchematicItem::ServerCraftSchematic slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Stripped)
        {
            Hooks::SwapVTable(L"UFortSchematicItem::ServerCraftSchematic", VTable, Index, reinterpret_cast<void*>(&ServerCraftSchematicHK));
        }
    }
}

int32_t UFortSchematicItem::ServerCraftSchematicHK(UFortSchematicItem* This, AFortPlayerController* PlayerController, int32_t Count)
{
    return This->ServerCraftSchematic(PlayerController, Count);
}
