#include "pch.h"
#include "STW/FortGameModeEndless.h"
#include "Engine/World.h"
#include "FortGameState.h"
#include "GameplayTagContainer.h"
#include "Logging/LogMacros.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionLibrary.h"
#include "Missions/FortMissionManager.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "UObject/WeakObjectPtr.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortMission, Log, All);

namespace
{
    // The primary mission StartPrimaryMission went to, so each run starts it once.
    TWeakObjectPtr<AFortMission> StartedPrimaryMission;
}

void AFortGameModeEndless::HandleNewPlayerHasBegunPlay(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    // Core's: when Epic's server sent StartPrimaryMission is unknown. Frostnite's mission (Mission_Endless) waits for it in
    // HandleMissionEvent_StartPrimaryMission (7.10's only handler of that event; no other blueprint and no client code
    // sends it, so it came from this class's server code, which the client build doesn't have, see the header). Its
    // handler only calls ActivateMission: the Global Activate event that sets up the storm shield and the wave data
    // and starts the first wave's breather (Endless_Generic_WaveData's first BreatherBetweenWaves, 181 s, "Next wave
    // starts in:"); the first wave then turns the Burner on (HandleOnWaveStarted, Wave 1: SetGeneratorActive and its
    // fuel loss). So it is sent as soon as a player is in and playing, the moment the game tells the mission so
    // itself (NewPlayerHasBegunPlay, which Mission_Endless answers with the starting inventory). The mission was loaded
    // and activated (its BlueprintOnActivated found the Burner) while the world was made, before anyone could join.
    if (!PlayerController || Role != ROLE_Authority)
    {
        return;
    }
    StartPrimaryMission();
#endif
}

void AFortGameModeEndless::StartPrimaryMission()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    AFortMissionManager* MissionManager = GameState ? static_cast<AFortMissionManager*>(GameState->MissionManager) : nullptr;
    AFortMission* PrimaryMission = nullptr;
    if (MissionManager)
    {
        for (AFortMission* Mission : static_cast<TArray<AFortMission*>&>(MissionManager->Missions))
        {
            if (Mission && !Mission->IsPendingKill() && Mission->IsPrimaryMission())
            {
                PrimaryMission = Mission;
                break;
            }
        }
    }
    if (!PrimaryMission)
    {
        UE_LOG(LogFortMission, Warning, TEXT("Frostnite: a player began play, but the zone has no primary mission yet (mission manager %s); StartPrimaryMission waits for the next player"),
            MissionManager ? TEXT("found") : TEXT("missing"));
        return;
    }
    if (StartedPrimaryMission.Get() == PrimaryMission)
    {
        return;
    }

    StartedPrimaryMission = PrimaryMission;
    UE_LOG(LogFortMission, Log, TEXT("Frostnite: StartPrimaryMission for %s"), *PrimaryMission->GetName());
    UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"StartPrimaryMission"), PrimaryMission->MissionGuid, FGameplayTagContainer(), nullptr);
#endif
}
