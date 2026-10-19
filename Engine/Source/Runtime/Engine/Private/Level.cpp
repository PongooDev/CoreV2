#include "pch.h"
#include "Engine/Level.h"
#include "Engine/World.h"
#include "EngineLogs.h"

DEFINE_LOG_CATEGORY(LogLevel);

bool ULevel::HasVisibilityChangeRequestPending() const
{
    UWorld* World = OwningWorld;
    return World && (this == World->CurrentLevelPendingVisibility || this == World->CurrentLevelPendingInvisibility);
}
