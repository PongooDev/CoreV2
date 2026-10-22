#include "pch.h"
#include "AI/FortAIDirector.h"
#include "Engine/World.h"
#include "FortGameModeZone.h"
#include "Natives.h"
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

void AFortAIDirector::StopEncounter(UFortAIEncounterInfo* EncounterToStop, EFortObjectiveStatus EncounterStopReason, bool bForceKillAI, bool bBroadcastStop)
{
#if WITH_SERVER_CODE
    if (Natives::AFortAIDirector_StopEncounter)
    {
        Natives::AFortAIDirector_StopEncounter(this, reinterpret_cast<UObject*>(EncounterToStop), EncounterStopReason, bForceKillAI, bBroadcastStop);
    }
#endif
}
