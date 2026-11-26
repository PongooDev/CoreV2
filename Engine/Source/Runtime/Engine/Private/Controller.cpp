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

bool AController::SetControlRotation(const FRotator& NewRotation)
{
    Reflection::FParams Params(FindStaticFunction_SetControlRotation(), true);
    return Params.Set(L"NewRotation", NewRotation) && Params.Call(this);
}

bool AController::ClientSetRotation(const FRotator& NewRotation, bool bResetCamera)
{
    Reflection::FParams Params(FindStaticFunction_ClientSetRotation(), true);
    return Params.Set(L"NewRotation", NewRotation) && Params.Set(L"bResetCamera", bResetCamera) && Params.Call(this);
}
