#include "pch.h"
#include "Building/BuildingTrap.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Player/FortPlayerState.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortTrap, Log, All);

void ABuildingTrap::TakePlacerTeam()
{
    // A placed trap keeps the team it was spawned with, HumanCampaign (0): AFortDecoTool's spawn (1.7.2 0x91B370) leaves
    // the rest to two controller slots (0x29F, 0x2A0) that are empty in 1.7.2. A Battle Royale player is on a team of their
    // own, so their own trap took them for an enemy and hit them (#74). The placer is the player whose WorldPlayerId the
    // trap was initialized with.
    const int32_t OwnerID = OwnerPersistentID;
    UWorld* World = GetWorld();
    AFortPlayerState* Placer = nullptr;
    for (TObjectIterator<AFortPlayerState> It; It && !Placer && OwnerID >= 0; ++It)
    {
        Placer = It->GetWorld() == World && static_cast<int32_t>(It->WorldPlayerId) == OwnerID ? *It : nullptr;
    }
    if (!Placer)
    {
        UE_LOG(LogFortTrap, Warning, TEXT("%s: no player with world id %d placed it, its team stays %d"), *GetName(), OwnerID, static_cast<uint8_t>(Team));
        return;
    }

    // AFortPlayerStateAthena::TeamIndex is copied from the player's AFortTeamInfo when they join it.
    AFortTeamInfo* TeamInfo = Placer->PlayerTeam;
    AFortPlayerStateAthena* AthenaPlacer = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(Placer));
    if (!TeamInfo && !AthenaPlacer)
    {
        return;
    }
    Team = TeamInfo ? static_cast<EFortTeam>(TeamInfo->Team) : static_cast<EFortTeam>(AthenaPlacer->TeamIndex);
    ForceNetUpdate();
    UE_LOG(LogFortTrap, Log, TEXT("%s placed by %s is on their team %d"), *GetName(), *Placer->GetName(), static_cast<uint8_t>(Team));
}

void ABuildingTrap::HookVTables()
{
    if (!Finder::ResolveTrapInitializeLive())
    {
        return;
    }

    const int32_t Index = Natives::ABuildingTrap_InitializeKismetSpawnedBuildingActorIndex;
    void* const Initialize = reinterpret_cast<void*>(Natives::ABuildingTrap_InitializeKismetSpawnedBuildingActor);
    InitializeKismetSpawnedBuildingActorOG = reinterpret_cast<decltype(InitializeKismetSpawnedBuildingActorOG)>(Initialize);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Initialize)
        {
            Hooks::SwapVTable(L"ABuildingTrap::InitializeKismetSpawnedBuildingActor", VTable, Index, reinterpret_cast<void*>(&InitializeKismetSpawnedBuildingActorHK));
        }
    }
}

void ABuildingTrap::InitializeKismetSpawnedBuildingActorHK(ABuildingTrap* This, uint64_t InitialState, uint64_t InOwnerPersistentID, uint64_t Arg3, void* Arg4, uint64_t Arg5)
{
    InitializeKismetSpawnedBuildingActorOG(This, InitialState, InOwnerPersistentID, Arg3, Arg4, Arg5);

#if WITH_SERVER_CODE
    if (This->Role == ROLE_Authority && AFortGameModeAthena::GetCurrent(This))
    {
        This->TakePlacerTeam();
    }
#endif
}
