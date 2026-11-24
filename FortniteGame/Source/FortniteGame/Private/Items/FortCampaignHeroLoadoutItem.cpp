#include "pch.h"
#include "Items/FortCampaignHeroLoadoutItem.h"
#include "Abilities/FortAbilityKit.h"
#include "Heroes/FortHero.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UFortHero* UFortCampaignHeroLoadoutItem::GetCommanderHero() const
{
    Reflection::FParams Params(StaticFunction_GetCommanderHero());
    return Params.Call(const_cast<UFortCampaignHeroLoadoutItem*>(this)) ? Params.Take<UFortHero*>(L"ReturnValue") : nullptr;
}

UFortGadgetItemDefinition* UFortCampaignHeroLoadoutItem::GetGadgetDefinitionBySlotIndex(int32_t SlotIndex) const
{
    Reflection::FParams Params(StaticFunction_GetGadgetDefinitionBySlotIndex());
    Params.Set(L"SlotIndex", SlotIndex);
    return Params.Call(const_cast<UFortCampaignHeroLoadoutItem*>(this)) ? Params.Take<UFortGadgetItemDefinition*>(L"ReturnValue") : nullptr;
}

void UFortCampaignHeroLoadoutItem::DetermineHeroLoadoutGadgetsAndItems(TArray<UFortGadgetItemDefinition*>& OutGadgets, TStructArray<FAbilityKitItem>& OutItems) const
{
    if (const UFortHero* Commander = GetCommanderHero())
    {
        Commander->DetermineCommanderGadgetsAndItems(OutGadgets, OutItems);
    }

    const TStructArray<FFilledGadgetSlot>& FilledSlots = Gadgets;
    for (int32_t Index = 0; Index < FilledSlots.Num(); ++Index)
    {
        if (UFortGadgetItemDefinition* Gadget = GetGadgetDefinitionBySlotIndex(FilledSlots[Index].slot_index))
        {
            if (!OutGadgets.Contains(Gadget))
            {
                OutGadgets.Add(Gadget);
            }
        }
    }
}
