#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "StructArray.h"
#include "UObject/Object.h"

class UFortGadgetItemDefinition;
class UFortHero;
struct FAbilityKitItem;

struct FFilledGadgetSlot
{
    DefineUnrealStruct(FFilledGadgetSlot, FortniteGame)
    DefineUProperty(FString, Gadget)
    DefineUProperty(int32_t, slot_index)
};

// 8.00 (CL-5203069) on: a Save the World hero loadout from the campaign profile (commander, support squad, team perk and
// the slotted gadgets).
class UFortCampaignHeroLoadoutItem : public UObject
{
    DefineUnrealClass(UFortCampaignHeroLoadoutItem, FortniteGame)
    DefineUFunction(GetCommanderHero)
    DefineUFunction(GetGadgetDefinitionBySlotIndex)

public:
    DefineUStructArrayProperty(FFilledGadgetSlot, Gadgets)

    UFortHero* GetCommanderHero() const;
    UFortGadgetItemDefinition* GetGadgetDefinitionBySlotIndex(int32_t SlotIndex) const;
    // The commander's ability kit gadgets and items, then the slotted gadgets. 8.x's client does the same on its own
    // (8.40 AFortPlayerControllerPvE 0x16A3BF0 and 0x1697AD0) to slot the world items it finds for them.
    void DetermineHeroLoadoutGadgetsAndItems(TArray<UFortGadgetItemDefinition*>& OutGadgets, TStructArray<FAbilityKitItem>& OutItems) const;

    static bool IsAvailable() { return StaticClass() && FindStaticFunction_GetCommanderHero() && FindStaticFunction_GetGadgetDefinitionBySlotIndex() && Member_Gadgets.IsAvailable(); }
};
