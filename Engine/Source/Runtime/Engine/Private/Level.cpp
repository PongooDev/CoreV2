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

TArray<AActor*>& ULevel::GetActors() const
{
    // Actors is native; it sits right before OwningWorld.
    return *reinterpret_cast<TArray<AActor*>*>(reinterpret_cast<uintptr_t>(this) + Member_OwningWorld.GetOffsetChecked() - sizeof(TArray<AActor*>));
}
