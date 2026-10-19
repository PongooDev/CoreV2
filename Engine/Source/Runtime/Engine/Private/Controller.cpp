#include "pch.h"
#include "GameFramework/Controller.h"
#include "EngineLogs.h"
#include "Natives.h"

DEFINE_LOG_CATEGORY(LogPath);

AActor* AController::GetViewTarget() const
{
    return CallVirtual<AActor*>(this, Natives::AController_GetViewTargetIndex);
}
