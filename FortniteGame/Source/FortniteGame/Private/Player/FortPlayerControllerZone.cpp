#include "pch.h"
#include "Player/FortPlayerControllerZone.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "Hooks.h"
#include "Log.h"
#include "Missions/FortMissionLibrary.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState || GameState->GameplayState != EFortGameplayState::EndOfZone || SetupTeleportOutCameraGameState.Get() == GameState)
    {
        return;
    }

    SetupTeleportOutCameraGameState = FWeakObjectPtr(GameState);
    UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"SetupTeleportOutCamera", FNAME_Find), nullptr);
#endif
}

void AFortPlayerControllerZone::HookVTables()
{
    const int32_t Index = Finder::ImplementationIndex(StaticFunction_ServerTriggerSetupTeleportCameraEvent());
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerTriggerSetupTeleportCameraEvent_ImplementationHK));
        }
    }
}

void AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerZone* This)
{
    This->ServerTriggerSetupTeleportCameraEvent_Implementation();
}
