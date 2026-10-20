#include "pch.h"
#include "FortKismetLibrary.h"
#include "Abilities/FortAbilitySet.h"
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
