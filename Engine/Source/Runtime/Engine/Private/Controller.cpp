#include "pch.h"
#include "GameFramework/Controller.h"
#include "EngineLogs.h"
#include "Natives.h"
#include "ReflectionParams.h"

DEFINE_LOG_CATEGORY(LogPath);

AActor* AController::GetViewTarget() const
{
    return CallVirtual<AActor*>(this, Natives::AController_GetViewTargetIndex);
}

void AController::Possess(APawn* InPawn)
{
    Reflection::FParams Params(StaticFunction_Possess());
    Params.Set(L"InPawn", InPawn);
    Params.Call(this);
}

void AController::UnPossess()
{
    Reflection::FParams Params(StaticFunction_UnPossess());
    Params.Call(this);
}
