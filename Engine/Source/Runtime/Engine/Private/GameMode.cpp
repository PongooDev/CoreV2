#include "pch.h"
#include "GameFramework/GameMode.h"
#include "ReflectionParams.h"

void AGameMode::StartMatch()
{
    Reflection::FParams Params(StaticFunction_StartMatch());
    Params.Call(this);
}
