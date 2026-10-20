#include "pch.h"
#include "Items/FortQuickBars.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

void AFortQuickBars::EnableSlot(EFortQuickBars InQuickBar, int32_t SlotIndex)
{
    Reflection::FParams Params(StaticFunction_EnableSlot());
    Params.Set(L"InQuickBar", static_cast<uint8_t>(InQuickBar));
    Params.Set(L"SlotIndex", SlotIndex);
    Params.Call(this);
}

void AFortQuickBars::SetupQuickbar(EFortQuickBars InQuickBar, const FQuickBarData& QuickBarData)
{
#if WITH_SERVER_CODE
    FQuickBar& QuickBar = GetQuickBar(InQuickBar);
    FQuickBarData& DataDefinition = QuickBar.DataDefinition;
    FQuickBarData::StaticStruct()->CopyScriptStruct(&DataDefinition, &QuickBarData);

    FScriptArray& Slots = QuickBar.Slots;
    const int32_t NumSlots = DataDefinition.QuickbarSlots.Num();
    if (Slots.Num() < NumSlots)
    {
        Slots.AddZeroed(NumSlots - Slots.Num(), FQuickBarSlot::StaticStruct()->GetStructureSize());
    }

    for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
    {
        EnableSlot(InQuickBar, SlotIndex);
    }
#endif
}
