#pragma once

#include "GameFramework/Info.h"
#include "Reflection.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"

class AActor;
class AController;
class AFortPlayerControllerAthena;
class AFortPlayerPawn;
class AFortPlayerState;
class UFunction;
class UWorld;

// A Creative game (from 7.00, which moves 6.20's minigame state and its leaderboard into this one class): the game an
// island's devices set up (its stats, end conditions and teams), started and ended from the island menu
// (ServerStartMinigame and ServerEndMinigame, which call StartMinigame and EndMinigame). Its scores are its stats
// (AddStatToPlayer and the rest, intact natives): 7.00 has no score registry of its own (SpawnOrGetMinigameScoreRegistry
// is gone).
class AFortMinigame : public AInfo
{
    DefineUnrealClass(AFortMinigame, FortniteGame)
    DefineUFunction(GetParticipatingPlayers)
    DefineUFunction(DetermineSpawnLocation)
    DefineUFunction(FinishPlayerTeleportedForMinigameStart)
    DefineUFunction(TeleportPlayersToStartLocations)
    DefineUFunction(ClearPlayerInventories)
    DefineUFunction(ClearPlayerInventory)

public:
    // Broadcast by HandleMinigameStarted_Implementation (7.10 0x10B1830) as the game goes in progress, and by
    // HandleMinigameEnded as it stops being in progress (AFortMinigame::SetMinigameState, 7.10 0x10BE210), on the server.
    DefineUProperty(FMulticastScriptDelegate, MinigameStartedDelegate)
    DefineUProperty(FMulticastScriptDelegate, MinigameEndedDelegate)
    static bool HasStartedAndEndedDelegates() { return Member_MinigameStartedDelegate.IsAvailable() && Member_MinigameEndedDelegate.IsAvailable(); }

    // Restored native (7.00 strips it: its exec is the shared empty one the build gives every server-only native): at
    // the game's start each player goes to their start (DetermineSpawnLocation: the game's start pads for their team, the
    // sky or where they are, as the game's SpawnLocationSetting says), and the game hears they arrived
    // (FinishPlayerTeleportedForMinigameStart).
    void TeleportPlayersToStartLocations();
    // Restored natives (7.10 and 7.20 strip both; 7.00 doesn't have them: "Player inventory is now reset when a game is
    // started and also after it finishes" is 7.01's). Each player of the game loses what they picked up or took from the
    // Creative inventory: every item that can be dropped (UFortWorldItemDefinition::bCanBeDropped) and isn't one of the
    // game mode's StartingItems. What can't be dropped stays: the pickaxe, the building pieces, the edit tool and the
    // Creative phone, which the island's read-only state takes and gives back on its own (Athena_PlayerController's
    // HandlePermissions).
    void ClearPlayerInventories();
    void ClearPlayerInventory(AFortPlayerControllerAthena* PlayerController);

    DECLARE_FUNCTION(execTeleportPlayersToStartLocations);
    DECLARE_FUNCTION(execClearPlayerInventories);
    DECLARE_FUNCTION(execClearPlayerInventory);

    // From 7.00, where the build's exec is the empty one.
    static void HookNatives();
    // From 7.10, where ClearPlayerInventories was restored: the minigames of the world (each island volume's
    // UFortMinigameVolumeComponent spawns its own) call it from their MinigameStartedDelegate and MinigameEndedDelegate.
    static void BindWorld(UWorld* World);

private:
    static void BindInventoryReset(AFortMinigame* Minigame);
    static inline UFunction* ClearPlayerInventoriesFunction = nullptr;

    TArray<AFortPlayerState*> GetParticipatingPlayers();
    bool DetermineSpawnLocation(AController* PlayerController, FVector& OutLocation, FRotator& OutRotation, bool& bShouldSkydive);
    void FinishPlayerTeleportedForMinigameStart(AFortPlayerPawn* PlayerPawn);
};
