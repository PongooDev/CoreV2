#include "pch.h"
#include "GameFramework/Controller.h"
#include "EngineLogs.h"
#include "Natives.h"
#include "ReflectionParams.h"

#include <intrin.h>

DEFINE_LOG_CATEGORY(LogPath);
// TEMPORARY (#688), with AActor::bLogViewRotations.
DEFINE_LOG_CATEGORY_STATIC(LogViewRotation, Log, All);

extern "C" IMAGE_DOS_HEADER __ImageBase;

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
    if (bLogViewRotations)
    {
        UE_LOG(LogViewRotation, Log, TEXT("#688 Core ClientSetRotation: %s gets P %.2f Y %.2f R %.2f (reset camera %s), from Core 0x%llX"), *GetNameSafe(this),
            NewRotation.Pitch, NewRotation.Yaw, NewRotation.Roll, bResetCamera ? TEXT("yes") : TEXT("no"),
            static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(_ReturnAddress()) - reinterpret_cast<uintptr_t>(&__ImageBase)));
    }
    Reflection::FParams Params(FindStaticFunction_ClientSetRotation(), true);
    return Params.Set(L"NewRotation", NewRotation) && Params.Set(L"bResetCamera", bResetCamera) && Params.Call(this);
}
