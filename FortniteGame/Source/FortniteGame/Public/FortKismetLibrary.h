#pragma once

#include "UObject/Object.h"
#include "UObject/ScriptInterface.h"
#include "UObject/Stack.h"

class AFortAIDirector;
class AFortAIGoalManager;
class IAbilitySystemInterface;
class UFortAbilitySet;
struct FFortAbilitySetHandle;

class UFortKismetLibrary : public UObject
{
    DefineUnrealClass(UFortKismetLibrary, FortniteGame)
    DefineUFunction(EquipFortAbilitySet)
    DefineUFunction(GetAIDirector)
    DefineUFunction(GetAIGoalManager)

public:
    static void EquipFortAbilitySet(TScriptInterface<IAbilitySystemInterface> AbilitySystemInterfaceActor, UFortAbilitySet* AbilitySet, UObject* OverrideSourceObject, FFortAbilitySetHandle& OutHandle);
    static AFortAIDirector* GetAIDirector(UObject* WorldContextObject);
    static AFortAIGoalManager* GetAIGoalManager(UObject* WorldContextObject);

    DECLARE_FUNCTION(execGetAIDirector);
    DECLARE_FUNCTION(execGetAIGoalManager);

    static void HookNatives();
};
