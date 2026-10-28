#include "pch.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/Controller.h"
#include "ReflectionParams.h"

UClass* AGameModeBase::GetDefaultPawnClassForController(AController* InController)
{
    Reflection::FParams Params(StaticFunction_GetDefaultPawnClassForController());
    Params.Set(L"InController", InController);
    return Params.Call(this) ? Params.Take<UClass*>(L"ReturnValue") : nullptr;
}

void AGameModeBase::RestartPlayer(AController* NewPlayer)
{
    Reflection::FParams Params(StaticFunction_RestartPlayer());
    Params.Set(L"NewPlayer", NewPlayer);
    Params.Call(this);
}
