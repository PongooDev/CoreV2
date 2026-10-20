#pragma once

#include "UObject/Object.h"
#include "UObject/ScriptInterface.h"

class IAbilitySystemInterface;
class UFortAbilitySet;
struct FFortAbilitySetHandle;

class UFortKismetLibrary : public UObject
{
    DefineUnrealClass(UFortKismetLibrary, FortniteGame)
    DefineUFunction(EquipFortAbilitySet)

public:
    static void EquipFortAbilitySet(TScriptInterface<IAbilitySystemInterface> AbilitySystemInterfaceActor, UFortAbilitySet* AbilitySet, UObject* OverrideSourceObject, FFortAbilitySetHandle& OutHandle);
};
