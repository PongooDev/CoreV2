#pragma once

#include "Engine/NetSerialization.h"
#include "GameFramework/Info.h"
#include "GameFramework/OnlineReplStructs.h"
#include "ObjectLayout.h"
#include "Reflection.h"
#include "StructArray.h"
#include "UObject/Stack.h"

#include <cstdint>

class AFortPlayerState;
class FString;

// One recorded score (6.20, CL-4497486): the player (UniqueId, PlayerName), the score and when it was recorded.
struct FMinigameScoreEntry : public FFastArraySerializerItem
{
    DefineUnrealStruct(FMinigameScoreEntry, FortniteGame)

public:
    DefineUStructProperty(FUniqueNetIdRepl, UniqueId)
    DefineUProperty(FString, PlayerName)
    DefineUProperty(float, Score)
};

// A registry's scores: Scores replicates (each client builds its own HighScores from it in PostReplicatedAdd).
struct FMinigameScoreArray : public FFastArraySerializer
{
    DefineUnrealStruct(FMinigameScoreArray, FortniteGame)

public:
    DefineUStructArrayProperty(FMinigameScoreEntry, Scores)
    static int32_t ScoresOffset() { return Member_Scores.GetOffsetChecked(); }
};

// How a minigame ranks its scores: ScoreType (points or time), NumHighScores, bAscending (a time: lower is better).
struct FMinigameScoreTemplate
{
    DefineUnrealStruct(FMinigameScoreTemplate, FortniteGame)

public:
    DefineUProperty(int32_t, NumHighScores)
    DefineUProperty(bool, bAscending)
};

// A playset's leaderboard (6.20): UFortPlaysetItemDefinition::SpawnOrGetMinigameScoreRegistry spawns one per playset
// for the match, seeded with the playset's HighScorePresets rows, and the playset's minigame states share it
// (AFortMinigameState::ScoreRegistry). Its HUD (MinigameHUD_PostGame, MinigameHUD_HighScoreEntry) shows its high scores.
class AFortMinigameScoreRegistry : public AInfo
{
    DefineUnrealClass(AFortMinigameScoreRegistry, FortniteGame)
    DefineUFunction(RecordScore)

public:
    DefineUStructProperty(FMinigameScoreArray, Scores)
    // The minigame mode's ScoreTemplate, copied in when the registry is spawned.
    DefineUStructProperty(FMinigameScoreTemplate, ScoreTemplate)
    static bool HasScores() { return Member_Scores.IsAvailable() && Member_ScoreTemplate.IsAvailable() && FMinigameScoreEntry::StaticStruct(); }

    // Records PlayerState's Score: a new entry of Scores, dirty for replication, which goes into the high scores where it
    // ranks among the NumHighScores best. True when it does. Server only.
    bool RecordScore(AFortPlayerState* PlayerState, float Score);

    DECLARE_FUNCTION(execRecordScore);

    // From 6.20: RecordScore's native, where the build's exec calls a stub (6.20's and 6.21's do). Nothing on older builds.
    static void HookNatives();
};
