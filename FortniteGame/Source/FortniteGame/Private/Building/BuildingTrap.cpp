#include "pch.h"
#include "Building/BuildingTrap.h"
#include "Abilities/FortAbilitySet.h"
#include "AbilitySystemComponent.h"
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
    // own, so their own trap took them for an enemy and hit them (#74), and a jump pad, whose blueprint only triggers for
    // and launches its own team, launched no one (#87; LetEnemiesUse opens it to everyone). The trap itself only gets the
    // placer's WorldPlayerId, which stays -1 (AFortPlayerState 0x83C4A0) for every
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

void ABuildingTrap::LetEnemiesUse()
{
    // A damage trap leaves its own team's pawns out (ABuildingTrap's TriggerFilter, 1.8.2 0x486470) and stays that way. A
    // trap that its own team sets off is there to help: the Battle Royale jump pads (TID_Floor_Player_Jump_Pad_Athena and
    // its _Free_Direction twin, the pads 1.8.2's AthenaTraps loot list drops) are Save the World's
    // Trap_Floor_Player_Jump_Pad and Trap_Floor_Player_Jump_Free_Direction_Pad, which only trigger for their team
    // (TriggerFilter bExcludePawnEnemies), and whose ability (GA_Trap_FloorJumpPad, GA_Trap_FloorJumpPadDirectional) only
    // launches its team (its effect container's TargetFilter). Friendly means on the trap's team (FFortTargetFilter), so
    // on the placer's team only that team could use them (#87). Battle Royale's own Launch Pad (TID_Floor_Player_Launch_Pad_Athena,
    // 1.8.1 on, in the loot list from 1.9: AthenaTraps weight 0.5, 0 in 1.8.2) shows the intent. Its blueprint's
    // TriggerFilter leaves no team out (ABuildingTrap's constructor, 1.9 0x487A50, excludes only friends, and the blueprint
    // turns that off), and AFortLauncherAthena's trigger slot (1.8.2 0x43C650 slot 0x1B8, 1.9 0x43C290 slot 0x1B9) launches
    // every player pawn it finds that isn't down, without any ability. That's also why it never runs out: its ability set
    // (GAS_Trap_FloorJumpPad) is granted but never activated, so GE_TrapCost is never applied. Both filters here leave enemies in too;
    // the trap keeps its placer's team, so its own team is still friendly. A trap whose trigger already lets enemies in
    // is left as it is: that's the Launch Pad, and opening its ability set would only change the jump pads' shared class default.
    FFortTargetFilter& Filter = TriggerFilter;
    if (Filter.bExcludePawnFriends)
    {
        return;
    }
    if (!Filter.bExcludePawnEnemies)
    {
        UE_LOG(LogFortTrap, Log, TEXT("%s already lets everyone use it (team %d)"), *GetName(), static_cast<uint8_t>(Team));
        return;
    }

    Filter.bExcludePawnEnemies = false;
    int32_t Changed = 1;

    // The ability's class default, for the instance granted from it later, and the instances granted already: an
    // InstancedPerActor ability (GA_TrapGeneric) is copied from its class default when the trap's AbilitySet is granted.
    UFortAbilitySet* Set = AbilitySet;
    if (Set)
    {
        TArray<UClass*>& AbilityClasses = Set->GameplayAbilities;
        for (int32_t Index = 0; Index < AbilityClasses.Num(); ++Index)
        {
            UClass* AbilityClass = AbilityClasses[Index];
            UFortGameplayAbility* Default = AbilityClass ? Cast<UFortGameplayAbility>(AbilityClass->GetDefaultObject(false)) : nullptr;
            Changed += Default ? Default->IncludeEnemyTargets() : 0;
        }
    }

    UAbilitySystemComponent* AbilitySystem = AbilitySystemComponent;
    TStructArray<FGameplayAbilitySpec>* Specs = AbilitySystem ? &static_cast<TStructArray<FGameplayAbilitySpec>&>(AbilitySystem->ActivatableAbilities.Items) : nullptr;
    for (int32_t SpecIndex = 0; Specs && SpecIndex < Specs->Num(); ++SpecIndex)
    {
        FGameplayAbilitySpec& Spec = (*Specs)[SpecIndex];
        TArray<UGameplayAbility*>& Replicated = Spec.ReplicatedInstances;
        TArray<UGameplayAbility*>& NonReplicated = Spec.NonReplicatedInstances;
        for (TArray<UGameplayAbility*>* Instances : { &Replicated, &NonReplicated })
        {
            for (int32_t Index = 0; Index < Instances->Num(); ++Index)
            {
                UFortGameplayAbility* Ability = Cast<UFortGameplayAbility>(reinterpret_cast<UObject*>((*Instances)[Index]));
                Changed += Ability ? Ability->IncludeEnemyTargets() : 0;
            }
        }
    }

    UE_LOG(LogFortTrap, Log, TEXT("%s helps everyone, enemies of team %d included (%d filters opened)"), *GetName(), static_cast<uint8_t>(Team), Changed);
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
        Trap->LetEnemiesUse();
    }
#endif
}
