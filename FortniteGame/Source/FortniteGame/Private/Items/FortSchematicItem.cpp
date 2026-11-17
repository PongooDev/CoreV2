#include "pch.h"
#include "Items/FortSchematicItem.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortQuickBars.h"
#include "Items/FortSchematicItemDefinition.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

bool UFortAlterableItem::GetActiveAlterationTemplateIds(TArray<FString>& OutTemplateIds, TArray<UObject*>& OutDefinitions) const
{
    if (!Member_Alterations.IsAvailable() || !Member_AlterationInstances.IsAvailable() || !FFortItemEntry::HasAlterations())
    {
        return false;
    }

    const TArray<FString>& TemplateIds = Alterations;
    const TArray<UObject*>& Instances = AlterationInstances;
    const TArray<UObject*> Active = GetActiveAlterations();
    for (UObject* Definition : Active)
    {
        for (int32_t Index = 0; Definition && Index < Instances.Num() && Index < TemplateIds.Num(); ++Index)
        {
            if (Instances[Index] == Definition)
            {
                OutTemplateIds.Add(TemplateIds[Index]);
                OutDefinitions.Add(Definition);
                break;
            }
        }
    }
    return true;
}

void UFortSchematicItem::GetSchematicRequirements(TStructArray<FSchematicRequirement>& Requirements) const
{
    Reflection::FParams Params(StaticFunction_GetSchematicRequirements());
    if (Params.Call(const_cast<UFortSchematicItem*>(this)))
    {
        Params.Relocate(L"Requirements", &Requirements, sizeof(FScriptArray));
    }
}

FRecipe* UFortSchematicItem::GetSchematicRecipe(Reflection::FParams& Recipe) const
{
    return Recipe.Call(const_cast<UFortSchematicItem*>(this)) ? static_cast<FRecipe*>(Recipe.GetAddress(L"ReturnValue")) : nullptr;
}

int32_t UFortSchematicItem::GetSchematicQuantityProduced() const
{
    Reflection::FParams Params(StaticFunction_GetSchematicQuantityProduced());
    return Params.Call(const_cast<UFortSchematicItem*>(this)) ? Params.Take<int32_t>(L"ReturnValue") : 0;
}

bool UFortSchematicItem::TakesRequestedTier()
{
    static const bool bTakesTier = []
    {
        UFunction* Rpc = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerController:ServerCraftSchematic");
        return Rpc && Rpc->FindPropertyByName(FName(L"RequestedTier", FNAME_Find)) != nullptr;
    }();
    return bTakesTier;
}

bool UFortSchematicItem::CanCraftSchematic(AFortPlayerController* PlayerController, uint8_t& OutFailCause, bool bConsiderReservedItems, int32_t Count, uint8_t RequestedTier)
{
    if (!Natives::UFortSchematicItem_CanCraftSchematic && !Finder::ResolveCraftingLive())
    {
        return false;
    }
    // 7.10 (CL-4667333, 0x109FA70) takes the tier before the count: (PlayerController, OutFailCause,
    // bConsiderReservedItems, Tier, Count). The tier picks the definition whose recipe is checked (0x10AFA20: the
    // schematic's own for 0 or a tier at or above its own, else a lower tier's) and the count multiplies each cost. Its
    // callers pass both ([rsp+20h] the tier, [rsp+28h] the count: 0x12B2A40, 0x10BD54E); 7.00's (0x106FAC0) take the count
    // at [rsp+20h].
    if (TakesRequestedTier())
    {
        using FCanCraftSchematicWithTier = bool (*)(UFortSchematicItem*, AFortPlayerController*, uint8_t*, bool, int32_t, int32_t);
        return reinterpret_cast<FCanCraftSchematicWithTier>(Natives::UFortSchematicItem_CanCraftSchematic)(this, PlayerController, &OutFailCause, bConsiderReservedItems, RequestedTier, Count);
    }
    return Natives::UFortSchematicItem_CanCraftSchematic(this, PlayerController, &OutFailCause, bConsiderReservedItems, Count);
}

int32_t UFortSchematicItem::ServerCraftSchematic(AFortPlayerController* PlayerController, int32_t PostCraftSlot, int32_t CraftAmount, uint8_t RequestedTier)
{
#if WITH_SERVER_CODE
    // The RPC (ItemId, PostCraftSlot, bIsQuickCrafted in every build Core supports, and RequestedTier from 7.10) crafts
    // once. The slot it passes on is where the item goes, never an amount: read as one, a quick bar slot above 0 crafted
    // and charged that many times.
    uint8_t FailCause = 0;
    const int32_t Count = CraftAmount > 0 ? CraftAmount : 1;
    // 7.10's Down-Tier Crafting: a tier below the schematic's own crafts the lower tier's item from its own recipe. Core
    // only crafts the schematic's own tier, so a lower one is refused rather than charged and given as the higher.
    const UFortItemDefinition* OwnDefinition = GetItemDefinitionBP();
    if (RequestedTier != 0 && OwnDefinition && UFortItemDefinition::HasTier() && RequestedTier < static_cast<uint8_t>(OwnDefinition->Tier))
    {
        UE_LOG(LogFort, Warning, TEXT("ServerCraftSchematic: %s at tier %d, below its own %d: down-tier crafting isn't restored yet"), *GetName(), static_cast<int32_t>(RequestedTier),
            static_cast<int32_t>(static_cast<uint8_t>(OwnDefinition->Tier)));
        return 0;
    }
    if (!PlayerController || !CanCraftSchematic(PlayerController, FailCause, false, Count, RequestedTier))
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
        // Crafts what's missing of an ingredient from its own schematic, then takes the ingredient if it's consumed.
        const auto SpendIngredient = [PlayerController, Count](UFortWorldItemDefinition* IngredientDefinition, int32_t PerCraft, bool bIsConsumed)
        {
            const int32_t Needed = PerCraft * Count;
            if (!IngredientDefinition || Needed <= 0)
            {
                return;
            }

            const int32_t Owned = PlayerController->GetItemQuantity(IngredientDefinition);
            if (Owned < Needed)
            {
                if (UFortSchematicItem* IngredientSchematic = PlayerController->GetSchematicForItem(IngredientDefinition))
                {
                    const int32_t IngredientProduced = std::max(IngredientSchematic->GetSchematicQuantityProduced(), 1);
                    IngredientSchematic->ServerCraftSchematic(PlayerController, -1, (Needed - Owned + IngredientProduced - 1) / IngredientProduced);
                }
            }

            if (bIsConsumed)
            {
                UFortKismetLibrary::K2_RemoveItemFromPlayer(PlayerController, IngredientDefinition, Needed);
            }
        };

        if (UFunction* GetRecipe = FindStaticFunction_GetSchematicRecipe())
        {
            // 1.11 has no GetSchematicRequirements: its recipe's costs, read as 1.11's CanCraftSchematic (0x6CEC30) reads
            // them, each cost's item loaded from its soft pointer. Every cost is taken: CanCraftSchematic and the reserved
            // count of a craft in progress (2.1 0x6CF310, 0x893250) never read the recipe's bIsConsumed, which the trap and
            // ammo rows of CraftingRecipes_New leave false. Reading it made crafting free: the client's reservation ended with the
            // craft and the ingredients were back (#193).
            Reflection::FParams RecipeFrame(GetRecipe, true);
            const FRecipe* Recipe = GetSchematicRecipe(RecipeFrame);
            TStructArray<FFortItemQuantityPair>* Costs = Recipe ? &Recipe->RecipeCosts : nullptr;
            for (int32_t Index = 0; Costs && Index < Costs->Num(); ++Index)
            {
                FFortItemQuantityPair& Cost = (*Costs)[Index];
                const TAssetPtr<UFortItemDefinition>& CostItem = Cost.ItemDefinition;
                SpendIngredient(Cast<UFortWorldItemDefinition>(reinterpret_cast<UObject*>(CostItem.LoadSynchronous())), Cost.Quantity, true);
            }
        }
        else
        {
            TStructArray<FSchematicRequirement> Requirements;
            GetSchematicRequirements(Requirements);
            for (int32_t Index = 0; Index < Requirements.Num(); ++Index)
            {
                FSchematicRequirement& Requirement = Requirements[Index];
                SpendIngredient(Cast<UFortWorldItemDefinition>(reinterpret_cast<UObject*>(static_cast<UFortItemDefinition*>(Requirement.ItemDefinition))), Requirement.Count, Requirement.bIsConsumed);
            }
            FMemory::Free(Requirements.GetData());
        }
    }

    UFortQuestManager* QuestManager = PlayerController->GetQuestManager(ESubGame::GetCampaign());
    if (ResultDefinition->MaxStackSize > 1)
    {
        UFortKismetLibrary::K2_GiveItemToPlayer(PlayerController, ResultDefinition, QuantityProduced, true);
        if (QuestManager)
        {
            QuestManager->SendItemStatEvent(EFortQuestObjectiveStatEvent::GetCraft(), ResultDefinition, QuantityProduced);
        }
        return QuantityProduced;
    }

    AFortInventory* Inventory = PlayerController->WorldInventory;
    AFortQuickBars* Bars = PlayerController->QuickBars;
    const int32_t Level = GetLevel();
    // From 4.2 every weapon and trap schematic carries one perk per alteration slot (Core-Backend #270). The item crafted
    // from it carries the perks its level and rarity unlock, as the backend's CraftWorldItem gives them: the weapon then
    // applies them when it's equipped and a placed trap gets them, both from the item's GetActiveAlterations.
    TArray<FString> AlterationTemplateIds;
    TArray<UObject*> Alterations;
    GetActiveAlterationTemplateIds(AlterationTemplateIds, Alterations);
    for (int32_t Crafted = 0; Crafted < QuantityProduced; ++Crafted)
    {
        UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ResultDefinition->CreateTemporaryItemInstanceBP(1, Level));
        if (!Inventory || !WorldItem)
        {
            return Crafted;
        }

        if (Alterations.Num() > 0)
        {
            static_cast<FFortItemEntry&>(WorldItem->ItemEntry).SetAlterations(AlterationTemplateIds, Alterations);
        }
        WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
        Inventory->AddWorldItem(WorldItem);
        FFortItemEntry& NewEntry = WorldItem->ItemEntry;
        NewEntry.SetNewItemCount(1, true);

        // The first one goes to the primary quick bar slot the player crafted it into (FortInventoryContext's
        // CraftAndSlotSchematic) when that slot takes it and is free; otherwise, and for the rest, the first free slot.
        const EFortQuickBars Primary = EFortQuickBars::GetPrimary();
        const bool bPostCraftSlot = Crafted == 0 && Bars && PostCraftSlot >= 0 && PostCraftSlot < Bars->GetQuickBar(Primary).Slots.Num()
            && Bars->GetSlotData(Primary, PostCraftSlot).AcceptedItemTypes.Contains(ResultDefinition->ItemType)
            && Bars->GetSlot(Primary, PostCraftSlot).Items.Num() == 0;
        if (bPostCraftSlot)
        {
            Bars->ServerAddItemInternal(NewEntry.ItemGuid, Primary, PostCraftSlot);
        }
        else
        {
            PlayerController->TryAddToQuickBar(WorldItem);
        }
    }
    if (QuestManager)
    {
        QuestManager->SendItemStatEvent(EFortQuestObjectiveStatEvent::GetCraft(), ResultDefinition, QuantityProduced);
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

int32_t UFortSchematicItem::ServerCraftSchematicHK(UFortSchematicItem* This, AFortPlayerController* PlayerController, int32_t PostCraftSlot, uint8_t RequestedTier)
{
    // RequestedTier is only there from 7.10 (the game's own call, 0x12ECD30: Schematic, PlayerController, PostCraftSlot,
    // RequestedTier); before it r9 holds nothing of the call's.
    return This->ServerCraftSchematic(PlayerController, PostCraftSlot, 1, TakesRequestedTier() ? RequestedTier : 0);
}
