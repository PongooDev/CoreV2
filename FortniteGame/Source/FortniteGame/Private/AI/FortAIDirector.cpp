#include "pch.h"
#include "AI/FortAIDirector.h"
#include "Engine/World.h"
#include "FortGameModeZone.h"
#include "Templates/Casts.h"

AFortAIDirector* AFortAIDirector::GetCurrent(const UObject* WorldContextObject)
{
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    AFortGameModeZone* GameMode = World ? Cast<AFortGameModeZone>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    return GameMode ? GameMode->AIDirector : nullptr;
#else
    return nullptr;
#endif
}
