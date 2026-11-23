#pragma once

#include "Engine/NetSerialization.h"
#include "GameFramework/Info.h"
#include "Internationalization/Text.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Reflection.h"
#include "StructArray.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"

class AActor;
class AController;
class AFortPlayerControllerAthena;
class AFortPlayerPawn;
class AFortPlayerState;
class AFortPlayerStateAthena;
class AFortVolume;
class APawn;
class UFunction;
class UWorld;

// A Creative game's state (7.10: PreGame, Warmup, InProgress, PostGameEnd, PostGameAbandon).
class EFortMinigameState
{
    DefineUnrealEnum(EFortMinigameState, FortniteGame)
    DefineUEnumValue(PreGame)
    DefineUEnumValue(Warmup)
    DefineUEnumValue(InProgress)
    DefineUEnumValue(PostGameEnd)
    DefineUEnumValue(PostGameAbandon)
    // 8.20 (CL-5625478): PreGame, Setup, Transitioning, WaitingForCameras, Warmup, InProgress, PostGameEnd,
    // PostGameAbandon, PostGameReset. Read these only where HasTransitioning() says the build has them.
    DefineUEnumValue(Setup)
    DefineUEnumValue(Transitioning)
    DefineUEnumValue(PostGameReset)
    static bool HasTransitioning() { return StaticEnumInfo().FindValue(L"Transitioning") != Reflection::InvalidEnumValue; }
};

// A player of a Creative game (from 7.00), in the game's PlayerArray. 7.40 (CL-5046157) reads
// bIsTeleportingOrRespawningForGameplay (+0x37) in IsPlayerTeleportingOrRespawningForGameplay (0x11E05A0), which the
// island volume's own check counts as still in the island while it is set; ClearTeleportForMinigame and
// OnClientFinishTeleportingForMinigame clear it.
struct FMinigamePlayer : public FFastArraySerializerItem
{
    DefineUnrealStruct(FMinigamePlayer, FortniteGame)

public:
    DefineUProperty(AFortPlayerState*, PlayerState)
    DefineUProperty(bool, bIsTeleportingOrRespawningForGameplay)
    static bool HasTeleportingOrRespawning() { return Member_PlayerState.IsAvailable() && Member_bIsTeleportingOrRespawningForGameplay.IsAvailable(); }
    // Where the player was before the game started, which AddMinigamePlayer records (8.20 0x13185B0) for the
    // post-game reset.
    DefineUProperty(bool, bHasValidResetData)
    DefineUProperty(uint8_t, TeamBeforeMinigameStarted)
    DefineUProperty(FVector, LocationBeforeMinigameStarted)
    DefineUProperty(FRotator, RotationBeforeMinigameStarted)
    DefineUProperty(bool, bIsLocationBeforeGameStartedBlocked)
    static bool HasResetData() { return Member_bHasValidResetData.IsAvailable() && Member_TeamBeforeMinigameStarted.IsAvailable() && Member_LocationBeforeMinigameStarted.IsAvailable()
        && Member_RotationBeforeMinigameStarted.IsAvailable() && Member_bIsLocationBeforeGameStartedBlocked.IsAvailable(); }
};

struct FMinigamePlayers : public FFastArraySerializer
{
    DefineUnrealStruct(FMinigamePlayers, FortniteGame)

public:
    DefineUStructArrayProperty(FMinigamePlayer, Players)
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
    DefineUEnumValue(CurrentLocation)
};

class EFortMinigamePostGameSpawnLocationSetting
{
    DefineUnrealEnum(EFortMinigamePostGameSpawnLocationSetting, FortniteGame)
    DefineUEnumValue(PreGameLocation)
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
    // 7.40 (CL-5046157), which drops RespawnMinigamePlayers, TeleportPlayersToStartLocations and
    // FinishPlayerTeleportedForMinigameStart: the two ends of its client-acknowledged teleport, intact natives with no
    // caller or binding left in the image (Final, not Blueprint-callable; their binders went with the server code).
    DefineUFunction(OnClientFinishTeleportingForMinigame)
    DefineUFunction(ClearTeleportForMinigame)
    DefineUFunction(OnRep_CurrentState)
    DefineUFunction(StartMinigame)

public:
    // 7.40's (0x11EEAF0, intact; authority only): flips the game's auto restart (an unreflected flag, +0x651).
    DefineUFunction(ToggleAutoRestartMinigame)
    void ToggleAutoRestartMinigame();
    DefineUProperty(EFortMinigameWarmupType, WarmupType)
    DefineUProperty(EFortMinigamePlayerSpawnLocationSetting, SpawnLocationSetting)
    DefineUProperty(EFortMinigameState, CurrentState)
    DefineUProperty(AFortVolume*, Volume)
    // Who started the game last (StartMinigame_Implementation) or asked for its restart (RestartMinigame_Implementation).
    DefineUProperty(AFortPlayerState*, MinigameStarter)
    DefineUStructProperty(FMinigamePlayers, PlayerArray)
    // The travel screen's text at a game's start (Minigame_PlayerCreated: "Starting Game").
    DefineUProperty(FText, ClientMinigameStartedText)
    // Broadcast by SetMinigameState (7.10 0x10BE210) with each new state, on the server too.
    DefineUProperty(FMulticastScriptDelegate, StateChangedDelegate)
    // Broadcast by HandleMinigameStarted_Implementation (7.10 0x10B1830) as the game goes in progress, and by
    // HandleMinigameEnded as it stops being in progress (AFortMinigame::SetMinigameState, 7.10 0x10BE210), on the server.
    DefineUProperty(FMulticastScriptDelegate, MinigameStartedDelegate)
    DefineUProperty(FMulticastScriptDelegate, MinigameEndedDelegate)
    static bool HasStartedAndEndedDelegates() { return Member_MinigameStartedDelegate.IsAvailable() && Member_MinigameEndedDelegate.IsAvailable(); }
    // 7.40+: broadcast with the pawn by OnClientFinishTeleportingForMinigame (8.00 0x1228AD0, at 0x1228BFE) once a
    // player's teleport for the game ends. The Creative devices bind to it for a game's start: the Game Starting
    // Inventory's InitialInventorySetup (Trap_Floor_GameStartingInventory) gives its items there.
    DefineUProperty(FMulticastScriptDelegate, OnPawnTeleportedForMinigame)

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
    // build has the body; Core's reconstruction. 7.40 has no such UFunction: its warmup (0x11EE110) keeps only the
    // FName "MinigameStart" of its stripped Respawn branch, and the move goes through the travel screen
    // (UsesClientAcknowledgedTeleport).
    void RespawnMinigamePlayers();
    // 7.40's (CL-5046157) start of a game: the game's player teleport waits for the client (AFortMinigame::
    // DelayedTeleportPlayer, 0x11D0790, on FMinigamePlayer's timer: bServerIsReady, then again until bClientIsReady)
    // after the respawn preparation with RespawnCameraData's MinigameStart row and ClientMinigameStartedText, and the
    // game's OnClientFinishTeleportingForMinigame or ClearTeleportForMinigame ends it.
    static bool UsesClientAcknowledgedTeleport();
    // The end of 7.40's DelayedTeleportPlayer once both sides are ready (0x11D0790 returns there with nothing: its move is
    // stripped): the pawn goes to Location, and the game's own OnClientFinishTeleportingForMinigame (a skydiver: 0x11E1AF0
    // starts its skydive, clears the player's teleport, resets their DeathInfo, broadcasts OnPawnTeleportedForMinigame)
    // or ClearTeleportForMinigame (on the ground: 0x11CE480 clears the teleport) finishes it.
    void DelayedTeleportPlayer(AFortPlayerControllerAthena* PlayerController, const FVector& Location, const FRotator& Rotation, bool bRespawningOnGround);
    // Core's native for the minigame's StateChangedDelegate (UClass::AddNativeFunction): RespawnMinigamePlayers where
    // StartMinigame (7.10 0x10BFC70) called it, as SetMinigameState puts a Respawn game in Warmup; and, as it goes
    // InProgress after that warmup, ResumeSkydivingAfterWarmup for a game that spawns in the air. On 7.40 the warmup's
    // step runs on the next tick, after the island Blueprint's HandleMinigameWarmup has assigned the teams.
    void HandleStateChangedForWarmup(uint8_t NewState);
    // Core's (#502): the players a Respawn warmup held in the sky skydive again once the warmup's end releases them
    // (its SetStasisMode(None) leaves them walking on air). Epic's place, the island Blueprint's
    // OnPlayerTeleportedForMinigameStart with bWantsToSkydive, went with the stripped RespawnMinigamePlayers' caller.
    void ResumeSkydivingAfterWarmup();
    // Core's native for the volume manager's OnVolumeStateChanged (#543), as the game's island is Ready again after
    // AFortMinigame::PostGameReset's restore. 8.00's PostGameReset (0x122AAF0) lost 7.40's step that puts the game back
    // in PreGame (0x11E350F: CurrentState = PreGame, OnRep_CurrentState), so the game stayed post-game and couldn't
    // start (StartMinigame_Implementation needs PreGame or Warmup); a game still post-game here gets that step, and then
    // a restart RestartMinigame_Implementation left pending (bPendingRestart, or bAutoRestart) starts the game, as 8.20's
    // SetMinigameState does on PreGame (0x1341C80). Builds whose PostGameReset sets PreGame itself (7.40 and older) are
    // left alone.
    void HandleVolumeStateChangedForReset(uint8_t NewState, AFortVolume* ChangedVolume);

    // 8.20 (CL-5625478)'s state machine: SetMinigameState (0x13419C0) and AdvanceState (0x1319BC0, intact) run
    // PreGame -> Setup -> Transitioning -> WaitingForCameras -> Warmup -> InProgress, and EndMinigame (0x1323CE0) and
    // PostGameReset (0x1336860) PostGameEnd or PostGameAbandon -> PostGameReset. Its stripped steps: Transitioning's
    // case is empty (nobody is sent to their start and nothing advances it), PostGameResetPlayers and
    // ClearPlayerInventory have the shared empty exec, and OnVolumeStateChanged (0x1335AD0) only unbinds itself once
    // the island is Ready, so nothing takes PostGameReset back to PreGame.
    DefineUFunction(AdvanceState)
    DefineUFunction(PostGameResetPlayers)
    DefineUFunction(SetTeamForPlayer)
    DefineUFunction(GetSpawnLocationSetting)
    DefineUProperty(FName, MinigameStartCameraBehavior)
    DefineUProperty(FName, MinigameEndCameraBehavior)
    DefineUProperty(FText, ClientMinigameEndedText)
    DefineUProperty(EFortMinigamePostGameSpawnLocationSetting, PostGameSpawnLocationSetting)
    void AdvanceState();
    // Core's native for StateChangedDelegate on 8.20: Setup resets the players' inventories, Transitioning sends them to
    // their start (TransitionPlayersToMinigameStart), PostGameReset runs PostGameResetPlayers.
    void HandleStateChangedForTransition(uint8_t NewState);
    // Core's name for Transitioning's stripped case: each player not starting where they stand goes to the start
    // DetermineSpawnLocation picks, through the travel screen (MinigameStartCameraBehavior, ClientMinigameStartedText);
    // once none is still teleporting, AdvanceState (WaitingForCameras).
    void TransitionPlayersToMinigameStart();
    // Restored native (8.20 strips it): each player gets their team from before the game back, loses the game's items,
    // and goes back as PostGameSpawnLocationSetting says (PreGameLocation: where they were; IslandStart: the island's
    // start), through the travel screen (MinigameEndCameraBehavior, ClientMinigameEndedText).
    void PostGameResetPlayers();
    DECLARE_FUNCTION(execPostGameResetPlayers);
    DECLARE_FUNCTION(execHandleStateChangedForTransition);

    DECLARE_FUNCTION(execTeleportPlayersToStartLocations);
    DECLARE_FUNCTION(execClearPlayerInventories);
    DECLARE_FUNCTION(execClearPlayerInventory);
    DECLARE_FUNCTION(execRespawnMinigamePlayers);
    DECLARE_FUNCTION(execHandleStateChangedForWarmup);
    DECLARE_FUNCTION(execHandleVolumeStateChangedForReset);

    // From 7.00, where the build's exec is the empty one.
    static void HookNatives();
    // From 7.10, where ClearPlayerInventories was restored: the minigames of the world (each island volume's
    // UFortMinigameVolumeComponent spawns its own) call it from their MinigameStartedDelegate and MinigameEndedDelegate.
    static void BindWorld(UWorld* World);

private:
    static void BindGameEvents(AFortMinigame* Minigame);
    static inline UFunction* ClearPlayerInventoriesFunction = nullptr;
    static inline UFunction* HandleStateChangedForWarmupFunction = nullptr;
    static inline UFunction* HandleVolumeStateChangedForResetFunction = nullptr;
    static inline UFunction* HandleStateChangedForTransitionFunction = nullptr;
    // AdvanceState once Transitioning has no player still teleporting (or bForce, the failsafe's).
    void AdvanceTransitionWhenArrived(bool bForce);
    void AdvanceTransitionNextTick();
    uint8_t GetSpawnLocationSetting(const AFortPlayerState* PlayerState);
    bool SetTeamForPlayer(AFortPlayerState* PlayerState, uint8_t NewTeam);
    // bPendingRestart, the unreflected flag after MinigameStarter (-1 without it).
    static inline ptrdiff_t PendingRestartOffset = -1;

    // OutRotation and bOutRespawningOnGround where the build answers them (7.40: OutRotation from 7.30).
    bool OverridePlayerSpawnLocation(APawn* PlayerPawn, AFortPlayerStateAthena* PlayerState, FVector& OutLocation, FRotator* OutRotation = nullptr, bool* bOutRespawningOnGround = nullptr);

    TArray<AFortPlayerState*> GetParticipatingPlayers();
    bool DetermineSpawnLocation(AController* PlayerController, FVector& OutLocation, FRotator& OutRotation, bool& bShouldSkydive);
    void FinishPlayerTeleportedForMinigameStart(AFortPlayerPawn* PlayerPawn);
    void PutPlayerIntoStasis(APawn* PlayerPawn);
    void OnClientFinishTeleportingForMinigame(AFortPlayerPawn* InPawn);
    void ClearTeleportForMinigame(AFortPlayerPawn* InPawn);
    // The player's entry in PlayerArray (as 7.40's own lookups find it, 0x11CE480), or null.
    FMinigamePlayer* FindMinigamePlayer(const AFortPlayerState* PlayerState);
    // RespawnMinigamePlayers' move of one player on 7.40, through the travel screen. False where nothing was sent (the
    // build or the controller lacks the respawn preparation): the caller moves the pawn at once.
    bool StartTeleportForMinigameStart(AController* Controller, APawn* Pawn, const FVector& Location, const FRotator& Rotation, bool bRespawningOnGround);
};
