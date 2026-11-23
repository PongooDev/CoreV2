#pragma once

#include "Building/BuildingActor.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameplayEffect.h"
#include "StructArray.h"
#include "UObject/Stack.h"

class AActor;
class AFortGameStateAthena;
class AFortPlayerControllerAthena;
class UWorld;

class ESpawnMachineState
{
    DefineUnrealEnum(ESpawnMachineState, FortniteGame)
    DefineUEnumValue(OnCooldown)
};

// The Reboot Van (from 8.30, BGA_Athena_SCMachine_C; an ABuildingGameplayActor in Epic's tree). A dead player's
// Reboot Card (BGA_Athena_SCMachine_Pickup_C, which GAS_DefaultPlayer's GA_Athena_SCMachine_Passive spawns on the
// Event.Athena.Player.Died the pawn's death sends) adds their id to each squadmate's UAthenaResurrectionComponent when
// picked up. Holding interact on the van goes through the component's ServerOnBeginResurrectionInteraction (the dead
// teammates' cameras turn to ResurrectLocation, their bResurrectingNow is set); the finished interaction is the van's
// IFortInteractInterface::ServerOnAttemptInteract (8.30 0x1072210), which copies the ids, points the dead teammates'
// components at ResurrectLocation, calls OnResurrectionStarted and sets a timer of ResurrectionStartDelay. 8.30 binds
// that timer to the shared empty function (lea at 0x1072705): the reboot itself is stripped. The Blueprint does the
// rest from the events it is given: OnPlayerPawnResurrected (knock-back, effects, the pistol, ammo and wood) and
// OnResurrectionCompleted (Complete, then OnCooldown for GetCooldownLength, then WaitingForUse again).
class ABuildingGameplayActorSpawnMachine : public ABuildingActor
{
    DefineUnrealClass(ABuildingGameplayActorSpawnMachine, FortniteGame)

public:
    // An AFortPlayerStart: the van's ResurrectionLocation child actor (320 units over it).
    DefineUProperty(AActor*, ResurrectLocation)
    DefineUStructArrayProperty(FUniqueNetIdRepl, PlayerIdsForResurrection)
    DefineUStructProperty(FScalableFloat, ResurrectionNextPlayerDelay)
    static bool HasResurrection() { return Member_ResurrectLocation.IsAvailable() && Member_PlayerIdsForResurrection.IsAvailable() && Member_ResurrectionNextPlayerDelay.IsAvailable(); }
    DefineUProperty(uint8_t, SpawnMachineState)
    // The van's item in the game state's SpawnMachineRepData (0 until it has one).
    DefineUProperty(int32_t, IndexIntoGameState)
    DefineUFunction(SetSpawnMachineState)

    // Core's name (21.40 binds a lambda to StartResurrectionTimerHandle in ServerOnAttemptInteract): what the
    // interaction's timer runs once ResurrectionStartDelay is up. Each player whose id the van took and who is still
    // being rebooted comes back through the game's own respawn (RespawnPlayerAfterDeath: AFortGameModeAthena's
    // PlayerCanRestart and ChoosePlayerStart let a player with bResurrectingNow restart at their component's
    // ResurrectionLocation, and FinishRestartPlayer clears it), one every ResurrectionNextPlayerDelay; then
    // OnResurrectionCompleted.
    void StartResurrection();

    // Core's: what the van's server BeginPlay did and 8.30 strips (its BeginPlay, 0x104FC40, has nothing of it), for every
    // van in World once the match's playlist is final: ResurrectLocation from the van's ResurrectionLocation child actor
    // (without it neither the dead players' cameras nor their components get the place), and, where the match has
    // Reboot (the game state's IsResurrectionEnabled), the van's item in the game state's SpawnMachineRepData, which
    // the clients' map and the card carrier's van marker read. Run when the bus leaves, before anyone can use a van.
    static void SetUpSpawnMachines(UWorld* World);

    // From 8.30: StartResurrection where ServerOnAttemptInteract binds the stripped method (Hooks::RedirectAddress), and
    // SetSpawnMachineState's native, which keeps the van's SpawnMachineRepData item in step. Nothing on builds without
    // the van.
    static void HookNatives();

private:
    // Rebooting the player at Index of the van's PlayerIdsForResurrection, then the next one after
    // ResurrectionNextPlayerDelay, then OnResurrectionCompleted.
    void ResurrectPlayer(int32_t Index);
    void OnPlayerPawnResurrected(class APawn* PlayerPawn);
    void OnResurrectionCompleted();
    bool RebootPlayersInAir();

    // SetSpawnMachineState (8.30 exec 0x18359F0, body 0x1073F20) keeps the build's part (the state, OnSpawnMachineStateChanged,
    // ForceNetUpdate); the server's update of the van's SpawnMachineRepData item is stripped. Core's native runs the
    // build's and then that update: the item's state and, on cooldown, its start and end (GetCooldownLength).
    void UpdateSpawnMachineRepData();
    DECLARE_FUNCTION(execSetSpawnMachineState);
    static inline FNativeFuncPtr SetSpawnMachineStateNative = nullptr;
    AFortGameStateAthena* GetAthenaGameState() const;

    // The method the timer's delegate calls (this in rcx, as a member function's).
    static void StartResurrectionHK(ABuildingGameplayActorSpawnMachine* This);
};
