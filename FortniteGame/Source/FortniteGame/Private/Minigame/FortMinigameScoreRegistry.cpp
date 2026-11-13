#include "pch.h"
#include "Minigame/FortMinigameScoreRegistry.h"
#include "Finder.h"
#include "GameFramework/PlayerState.h"
#include "Log.h"
#include "Natives.h"
#include "Player/FortPlayerState.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"

#include <algorithm>
#include <vector>

bool AFortMinigameScoreRegistry::RecordScore(AFortPlayerState* PlayerState, float Score)
{
#if WITH_SERVER_CODE
    // 6.20 (CL-4497486) adds the Playground's leaderboards and the client builds strip this body: 6.20's exec (0x1575B10)
    // and 6.21's (0x158BA90) step the player state and the score and call the shared return-false stub. What is left
    // shows what it did (🟡 inferred from it):
    // - The registry's record of a score by name (Natives::AFortMinigameScoreRegistry_RecordScore, 6.21 0x1092680) is
    //   kept, for the playset's HighScorePresets: on the authority it adds an FMinigameScoreEntry with the name, the
    //   score, the time (UtcNow) and bHasScore, marks it dirty and inserts it in HighScores among NumHighScores
    //   (ScoreTemplate's bAscending order, an earlier time first on a tie); true when it ranks.
    // - The entries a player records carry their UniqueId too: GetScores(PlayerState) (6.21 0x1087A90) and
    //   RemoveAllScores(PlayerState) (0x10946C0) pick a player's entries by it.
    // So the player's name goes through the game's own record, and the entry it added takes the player's UniqueId.
    if (Role != ROLE_Authority || !PlayerState || !Natives::AFortMinigameScoreRegistry_RecordScore || !HasScores())
    {
        return false;
    }
    const FString PlayerName = reinterpret_cast<APlayerState*>(PlayerState)->GetPlayerName();
    const bool bHighScore = Natives::AFortMinigameScoreRegistry_RecordScore(this, PlayerName, Score);

    FMinigameScoreArray& ScoreArray = Scores;
    TStructArray<FMinigameScoreEntry>& Entries = ScoreArray.Scores;
    if (Entries.Num() > 0)
    {
        FMinigameScoreEntry& Entry = Entries[Entries.Num() - 1];
        if (UScriptStruct* NetIdStruct = FUniqueNetIdRepl::StaticStruct())
        {
            NetIdStruct->CopyScriptStruct(&static_cast<FUniqueNetIdRepl&>(Entry.UniqueId), &static_cast<FUniqueNetIdRepl&>(reinterpret_cast<APlayerState*>(PlayerState)->UniqueId));
            ScoreArray.MarkItemDirty(Entry);
        }
    }
    Log::Info(L"Minigame: {} recorded {:.2f} for {}{}", GetName().ToStdString(), Score, PlayerName.ToStdString(), bHighScore ? L", a high score" : L"");
    return bHighScore;
#else
    return false;
#endif
}

DEFINE_FUNCTION(AFortMinigameScoreRegistry::execRecordScore)
{
    // 6.21's exec (0x158BA90): the player state, then the score; the native's bool is the return value.
    P_GET_OBJECT(AFortPlayerState, Z_Param_PlayerState);
    P_GET_PROPERTY(UFloatProperty, Z_Param_Score);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<bool*>(RESULT_PARAM) = static_cast<AFortMinigameScoreRegistry*>(Context)->RecordScore(Z_Param_PlayerState, Z_Param_Score);
    P_NATIVE_END;
}

void AFortMinigameScoreRegistry::HookNatives()
{
#if WITH_SERVER_CODE
    // 6.20's Playground: older builds have no score registry.
    UFunction* Function = FindStaticFunction_RecordScore();
    if (!Function || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
    {
        Log::Info(L"Finder: AFortMinigameScoreRegistry::RecordScore is native in this build");
        return;
    }

    // The record by name: the call SpawnOrGetMinigameScoreRegistry's native makes for each HighScorePresets row, the one
    // whose target reads both the registry's ScoreTemplate (it passes it to the high scores' insert) and Scores.Scores'
    // count (it adds there).
    UFunction* SpawnOrGet = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlaysetItemDefinition:SpawnOrGetMinigameScoreRegistry");
    uint8_t* SpawnOrGetCall = SpawnOrGet ? Finder::ImplementationCall(SpawnOrGet) : nullptr;
    const uintptr_t SpawnOrGetNative = SpawnOrGetCall ? Finder::RelativeTarget(reinterpret_cast<uintptr_t>(SpawnOrGetCall), 1, 5) : 0;
    if (!HasScores() || !SpawnOrGetNative)
    {
        Log::Error(L"Hook AFortMinigameScoreRegistry::RecordScore: not restored (Scores, ScoreTemplate or SpawnOrGetMinigameScoreRegistry not found), so no minigame score reaches its leaderboard");
        return;
    }
    const int32_t TemplateOffset = Member_ScoreTemplate.GetOffsetChecked();
    const int32_t CountOffset = Member_Scores.GetOffsetChecked() + FMinigameScoreArray::ScoresOffset() + 8;
    std::vector<uintptr_t> Candidates;
    for (const auto& [Site, Target] : Finder::DecodedDirectCalls(SpawnOrGetNative))
    {
        if (Target != SpawnOrGetNative && Finder::ReadsMemberAt(Target, TemplateOffset) && Finder::ReadsMemberAt(Target, CountOffset)
            && std::find(Candidates.begin(), Candidates.end(), Target) == Candidates.end())
        {
            Candidates.push_back(Target);
        }
    }
    if (Candidates.size() != 1)
    {
        Log::Error(L"Finder: AFortMinigameScoreRegistry's record by name not found off SpawnOrGetMinigameScoreRegistry (0x{:X}, {} candidates; ScoreTemplate +0x{:X}, Scores count +0x{:X})",
            SpawnOrGetNative - Finder::ImageBase(), Candidates.size(), TemplateOffset, CountOffset);
        return;
    }
    Natives::AFortMinigameScoreRegistry_RecordScore = reinterpret_cast<bool (*)(UObject*, const FString&, float)>(Candidates[0]);
    Function->SetNativeFunc(&execRecordScore);
    Log::Info(L"Finder: AFortMinigameScoreRegistry's record by name 0x{:X} (SpawnOrGetMinigameScoreRegistry 0x{:X})", Candidates[0] - Finder::ImageBase(), SpawnOrGetNative - Finder::ImageBase());
    Log::Info(L"Hook AFortMinigameScoreRegistry::RecordScore: native");
#endif
}
