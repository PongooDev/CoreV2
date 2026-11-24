#include "pch.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Abilities/FortAbilityKit.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UFortHeroType* UFortHero::GetHeroTypeBP() const
{
    Reflection::FParams Params(StaticFunction_GetHeroTypeBP());
    return Params.Call(const_cast<UFortHero*>(this)) ? Params.Take<UFortHeroType*>(L"ReturnValue") : nullptr;
}

bool UFortHero::DetermineCommanderGadgetsAndItems(TArray<UFortGadgetItemDefinition*>& OutGadgets, TStructArray<FAbilityKitItem>& OutItems) const
{
    if (!CanDetermineCommanderGadgetsAndItems())
    {
        return false;
    }
    Natives::UFortHero_DetermineCommanderGadgetsAndItems(this, OutGadgets, reinterpret_cast<FScriptArray&>(OutItems));
    return true;
}

bool UFortHero::CanDetermineCommanderGadgetsAndItems()
{
    return Natives::UFortHero_DetermineCommanderGadgetsAndItems != nullptr;
}
