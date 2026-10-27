#include "pch.h"
#include "Building/BuildingTrap.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Finder.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortTrap, Log, All);

void ABuildingTrap::TakePlacerTeam(AFortPlayerController* Placer)
{
    // A placed trap keeps the team it was spawned with, HumanCampaign (0): AFortDecoTool's spawn (1.7.2 0x91B370) leaves
    // the rest to two controller slots (0x29F, 0x2A0) that are empty in 1.7.2. A Battle Royale player is on a team of their
    // own, so their own trap took them for an enemy and hit them (#74), and a jump pad, which only triggers for and
    // launches its own team (TriggerFilter and GA_Trap_FloorJumpPad's DamageTargetFilter exclude enemies), launched no one
    // (#87). The trap itself only gets the placer's WorldPlayerId, which stays -1 (AFortPlayerState 0x83C4A0) for every
    // Battle Royale player: Core sets it in its Zone Login hook, which Athena's own Login (slot 0xE0, 0x3F2450) never
    // reaches. The second slot, which gets the placer's controller once the trap is initialized, gives it their team.
    AFortPlayerState* PlayerState = Placer ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Placer->PlayerState))) : nullptr;

    // AFortPlayerStateAthena::TeamIndex is copied from the player's AFortTeamInfo when they join it.
    AFortTeamInfo* TeamInfo = PlayerState ? static_cast<AFortTeamInfo*>(PlayerState->PlayerTeam) : nullptr;
    AFortPlayerStateAthena* AthenaPlayerState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
    if (!TeamInfo && !AthenaPlayerState)
    {
        UE_LOG(LogFortTrap, Warning, TEXT("%s: its placer %s has no team, its team stays %d"), *GetName(), *GetNameSafe(Placer), static_cast<uint8_t>(Team));
        return;
    }
    Team = TeamInfo ? static_cast<EFortTeam>(TeamInfo->Team) : static_cast<EFortTeam>(AthenaPlayerState->TeamIndex);
    ForceNetUpdate();
    UE_LOG(LogFortTrap, Log, TEXT("%s placed by %s is on their team %d"), *GetName(), *GetNameSafe(Placer), static_cast<uint8_t>(Team));
}

void ABuildingTrap::HookVTables()
{
    if (!Finder::ResolveDecoSpawnedLive())
    {
        return;
    }

    const int32_t Index = Natives::AFortPlayerController_DecoSpawnedIndex;
    void* const Empty = Natives::SharedEmptyFunction;
    DecoSpawnedOG = reinterpret_cast<decltype(DecoSpawnedOG)>(Empty);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(AFortPlayerController::StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Empty)
        {
            Hooks::SwapVTable(L"AFortPlayerController deco spawned", VTable, Index, reinterpret_cast<void*>(&DecoSpawnedHK));
        }
    }
}

void ABuildingTrap::DecoSpawnedHK(AFortPlayerController* Placer, ABuildingSMActor* Spawned, void* Arg)
{
    DecoSpawnedOG(Placer, Spawned, Arg);

#if WITH_SERVER_CODE
    ABuildingTrap* Trap = Cast<ABuildingTrap>(reinterpret_cast<UObject*>(Spawned));
    if (Trap && Trap->Role == ROLE_Authority && AFortGameModeAthena::GetCurrent(Trap))
    {
        Trap->TakePlacerTeam(Placer);
    }
#endif
}
