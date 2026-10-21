#include "pch.h"
#include "FortGameMode.h"
#include "CoreGlobals.h"
#include "Finder.h"
#include "Hooks.h"
#include "Natives.h"
#include "Online/FortGameSessionDedicated.h"

TSubclassOf<AGameSession> AFortGameMode::GetGameSessionClass() const
{
    if (IsRunningDedicatedServer())
    {
        return AFortGameSessionDedicated::StaticClass();
    }

    return AFortGameSession::StaticClass();
}

APawn* AFortGameMode::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
    return Natives::AFortGameMode_SpawnDefaultPawnFor(this, NewPlayer, StartSpot);
}

void AFortGameMode::FinishWorldInitialization(AFortWorldManager* WorldManager)
{
    Natives::AFortGameMode_FinishWorldInitialization(this, WorldManager);
}

void AFortGameMode::SetBadgeScores()
{
    if (Natives::AFortGameMode_SetBadgeScores || Finder::ResolveBadgeScoresLive())
    {
        Natives::AFortGameMode_SetBadgeScores(this);
    }
}

void AFortGameMode::Hook()
{
    Hooks::Detour(L"AFortGameMode::GetGameSessionClass", reinterpret_cast<void*>(Natives::AFortGameMode_GetGameSessionClass), &GetGameSessionClassHK, nullptr);
}

TSubclassOf<AGameSession>* AFortGameMode::GetGameSessionClassHK(const AFortGameMode* This, TSubclassOf<AGameSession>* Result)
{
    *Result = This->GetGameSessionClass();
    return Result;
}
