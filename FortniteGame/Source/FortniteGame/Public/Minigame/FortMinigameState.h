#pragma once

#include "Engine/NetSerialization.h"
#include "GameFramework/Info.h"
#include "ObjectLayout.h"
#include "Reflection.h"
#include "StructArray.h"
#include "UObject/ScriptDelegates.h"
#include "UObject/Stack.h"

class AActor;
class AFortMinigameScoreRegistry;
class AFortPlayerState;
class UFunction;
class UWorld;

// A player in a minigame (6.20, CL-4497486): their score, bFinalScore once the minigame mode recorded their final one
// (RecordPlayerScore with bFinalScore), and bRecordedScore (not replicated) once that score went to the leaderboard.
struct FMinigamePlayerEntry : public FFastArraySerializerItem
{
    DefineUnrealStruct(FMinigamePlayerEntry, FortniteGame)

public:
    DefineUProperty(AFortPlayerState*, PlayerState)
    DefineUProperty(float, Score)
    DefineBitfieldUProperty(bFinalScore)
    DefineBitfieldUProperty(bRecordedScore)
};

struct FMinigamePlayerArray : public FFastArraySerializer
{
    DefineUnrealStruct(FMinigamePlayerArray, FortniteGame)

public:
    DefineUStructArrayProperty(FMinigamePlayerEntry, Players)
};

// A playset's minigame (6.20): its players and scores, its state (WaitingToStart, InProgress, WaitingPostMatch...) and the
// leaderboard it shares with the playset's other games (ScoreRegistry). The minigame mode (AFortMinigameMode, the
// authority's) spawns it.
class AFortMinigameState : public AInfo
{
    DefineUnrealClass(AFortMinigameState, FortniteGame)

public:
    DefineUProperty(AFortMinigameScoreRegistry*, ScoreRegistry)
    DefineUStructProperty(FMinigamePlayerArray, PlayerArray)
    // Broadcast by HandleMinigameStateChanged (6.21 0x1089C20) when the minigame stops being in progress, on the server
    // too: after EndMinigame has recorded every player's final score and moved the state to WaitingPostMatch.
    DefineUProperty(FMulticastScriptDelegate, MinigameEndedDelegate)

    // Each player's final score that the minigame mode recorded this game (bRecordedScore), to the leaderboard.
    void RecordFinalScores();

    DECLARE_FUNCTION(execRecordFinalScores);

    // From 6.20: Core's RecordFinalScores, for the minigame states' MinigameEndedDelegate to call.
    static void HookNatives();
    // Binds each minigame state the world spawns from now on (the Playground's playsets spawn theirs as they load).
    static void BindWorld(UWorld* World);

private:
    static void HandleActorSpawned(AActor* Actor);
    static inline UFunction* RecordFinalScoresFunction = nullptr;
};
