#include "pch.h"
#include "FortWorldManager.h"
#include "Engine/World.h"
#include "FortGameMode.h"
#include "Hooks.h"
#include "Natives.h"
#include "Online/FortGameSession.h"
#include "Online/FortLobbyBeaconHost.h"
#include "Templates/Casts.h"

void AFortWorldManager::OnWorldRecordQueryComplete()
{
    OnWorldRecordQueryCompleteOG(this);

#if WITH_SERVER_CODE
    if (WorldManagerState != EFortWorldManagerState::WMS_WorldQueryComplete)
    {
        return;
    }

    UWorld* World = GetWorld();
    AFortGameMode* GameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    AFortGameSession* GameSession = GameMode ? GameMode->FortGameSession : nullptr;
    if (AFortLobbyBeaconHost* LobbyBeaconHost = GameSession ? GameSession->LobbyBeaconHost : nullptr)
    {
        LobbyBeaconHost->SetWorldRecordLoaded(true);
    }
#endif
}

void AFortWorldManager::Hook()
{
    Hooks::Detour(L"AFortWorldManager::OnWorldRecordQueryComplete", reinterpret_cast<void*>(Natives::AFortWorldManager_OnWorldRecordQueryComplete), &OnWorldRecordQueryCompleteHK, &OnWorldRecordQueryCompleteOG);
}

void AFortWorldManager::OnWorldRecordQueryCompleteHK(AFortWorldManager* This)
{
    This->OnWorldRecordQueryComplete();
}
