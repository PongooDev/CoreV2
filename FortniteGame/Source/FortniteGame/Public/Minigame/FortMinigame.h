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
class AFortPlayerStateAthena;
class APawn;
class UFunction;
class UWorld;

// A Creative game's state (7.10: PreGame, Warmup, InProgress, PostGameEnd, PostGameAbandon).
class EFortMinigameState
{
    DefineUnrealEnum(EFortMinigameState, FortniteGame)
    DefineUEnumValue(Warmup)
};

// What a game's warmup does with its players (7.10: Stasis, Respawn; the island games' Minigame_PlayerCreated has
// Respawn and a 6 s WarmupDuration).
class EFortMinigameWarmupType
{
    DefineUnrealEnum(EFortMinigameWarmupType, FortniteGame)
    DefineUEnumValue(Respawn)
};

class EFortMinigamePlayerSpawnLocationSetting
{
    DefineUnrealEnum(EFortMinigamePlayerSpawnLocationSetting, FortniteGame)
    DefineUEnumValue(Air)
};

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
    DefineUFunction(RespawnMinigamePlayers)
    DefineUFunction(OverridePlayerSpawnLocation)
    DefineUFunction(PutPlayerIntoStasis)

public:
    DefineUProperty(EFortMinigameWarmupType, WarmupType)
    DefineUProperty(EFortMinigamePlayerSpawnLocationSetting, SpawnLocationSetting)
    // Broadcast by SetMinigameState (7.10 0x10BE210) with each new state, on the server too.
    DefineUProperty(FMulticastScriptDelegate, StateChangedDelegate)
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
    // Creative inventory: ClearPlayerInventory hands each to the player's own AFortPlayerControllerAthena::
    // ClearDroppableItems (every item that can be dropped and isn't one of the game mode's StartingItems).
    void ClearPlayerInventories();
    void ClearPlayerInventory(AFortPlayerControllerAthena* PlayerController);
    // Restored native (7.10 and 7.20 strip it; 7.00 doesn't have it, its island Blueprint calls
    // TeleportPlayersToStartLocations instead): a game whose WarmupType is Respawn puts each of its players where the
    // game's respawn path would (OverridePlayerSpawnLocation: their latest checkpoint, a spawn pad for their team, or the
    // air) as its warmup starts, and holds them there until the warmup ends (PutPlayerIntoStasis, the game's own). No
    // build has the body; Core's reconstruction.
    void RespawnMinigamePlayers();
    // Core's native for the minigame's StateChangedDelegate (UClass::AddNativeFunction): RespawnMinigamePlayers where
    // StartMinigame (7.10 0x10BFC70) called it, as SetMinigameState puts a Respawn game in Warmup.
    void HandleStateChangedForWarmup(uint8_t NewState);

    DECLARE_FUNCTION(execTeleportPlayersToStartLocations);
    DECLARE_FUNCTION(execClearPlayerInventories);
    DECLARE_FUNCTION(execClearPlayerInventory);
    DECLARE_FUNCTION(execRespawnMinigamePlayers);
    DECLARE_FUNCTION(execHandleStateChangedForWarmup);

    // From 7.00, where the build's exec is the empty one.
    static void HookNatives();
    // From 7.10, where ClearPlayerInventories was restored: the minigames of the world (each island volume's
    // UFortMinigameVolumeComponent spawns its own) call it from their MinigameStartedDelegate and MinigameEndedDelegate.
    static void BindWorld(UWorld* World);

private:
    static void BindGameEvents(AFortMinigame* Minigame);
    static inline UFunction* ClearPlayerInventoriesFunction = nullptr;
    static inline UFunction* HandleStateChangedForWarmupFunction = nullptr;

    bool OverridePlayerSpawnLocation(APawn* PlayerPawn, AFortPlayerStateAthena* PlayerState, FVector& OutLocation);

    TArray<AFortPlayerState*> GetParticipatingPlayers();
    bool DetermineSpawnLocation(AController* PlayerController, FVector& OutLocation, FRotator& OutRotation, bool& bShouldSkydive);
    void FinishPlayerTeleportedForMinigameStart(AFortPlayerPawn* PlayerPawn);
    void PutPlayerIntoStasis(APawn* PlayerPawn);
};
