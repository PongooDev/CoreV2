#pragma once

#include "GameFramework/Info.h"
#include "Reflection.h"
#include "UObject/Stack.h"

class AController;
class AFortPlayerPawn;
class AFortPlayerState;

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

public:
    // Restored native (7.00 strips it: its exec is the shared empty one the build gives every server-only native): at
    // the game's start each player goes to their start (DetermineSpawnLocation: the game's start pads for their team, the
    // sky or where they are, as the game's SpawnLocationSetting says), and the game hears they arrived
    // (FinishPlayerTeleportedForMinigameStart).
    void TeleportPlayersToStartLocations();

    DECLARE_FUNCTION(execTeleportPlayersToStartLocations);

    // From 7.00, where the build's exec is the empty one.
    static void HookNatives();

private:
    TArray<AFortPlayerState*> GetParticipatingPlayers();
    bool DetermineSpawnLocation(AController* PlayerController, FVector& OutLocation, FRotator& OutRotation, bool& bShouldSkydive);
    void FinishPlayerTeleportedForMinigameStart(AFortPlayerPawn* PlayerPawn);
};
