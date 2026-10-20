#include "pch.h"
#include "FortKismetLibrary.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIGoalManager.h"
#include "Abilities/FortAbilitySet.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"
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

void UFortKismetLibrary::HookNatives()
{
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }

    for (const auto& [Function, Native, Name] : { std::tuple{ StaticFunction_GetAIDirector(), &execGetAIDirector, L"UFortKismetLibrary::GetAIDirector" }, std::tuple{ StaticFunction_GetAIGoalManager(), &execGetAIGoalManager, L"UFortKismetLibrary::GetAIGoalManager" } })
    {
        if (Function)
        {
            Function->SetNativeFunc(Native);
            Log::Info(L"Hook {}: native", Name);
        }
    }
}
