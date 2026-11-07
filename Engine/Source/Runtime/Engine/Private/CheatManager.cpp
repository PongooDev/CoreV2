#include "pch.h"
#include "GameFramework/CheatManager.h"
#include "GameFramework/PlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"

APlayerController* UCheatManager::GetOuterAPlayerController() const
{
    return Cast<APlayerController>(GetOuter());
}

void UCheatManager::CheatScript(const FString& ScriptName)
{
    Reflection::FParams Params(StaticFunction_CheatScript());
    Params.Set(L"ScriptName", ScriptName);
    Params.Call(this);
}
