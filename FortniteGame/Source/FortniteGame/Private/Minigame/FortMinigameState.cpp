#include "pch.h"
#include "Minigame/FortMinigameState.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Log.h"
#include "Minigame/FortMinigameScoreRegistry.h"
#include "Natives.h"
#include "Player/FortPlayerState.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <set>
#include <utility>

namespace
{
    // The entries whose final score went to the leaderboard: (the minigame state, the entry's ReplicationID). A new
    // game's entries are new ones (ResetMinigame empties PlayerArray, 6.21 0x1094840), with new ids.
    std::set<std::pair<const UObject*, int32_t>> RecordedEntries;
    TWeakObjectPtr<UWorld> BoundWorld;
}

void AFortMinigameState::RecordFinalScores()
{
#if WITH_SERVER_CODE
    // 6.20 (CL-4497486) strips AFortMinigameScoreRegistry::RecordScore, and the compiler folded its one native call away:
    // the minigame mode's record of a player's score (6.21 0x1093070, from RecordPlayerScore 0x1092E60 and from
    // EndMinigame's every-player pass 0x1093200) ends `if (ScoreRegistry && bFinalScore && !bRecordedScore)
    // { bRecordedScore = true; <RecordScore, gone>; PlayerArray.MarkItemDirty(Entry); }` (0x10931B2-0x10931D2), so a
    // finished game never reached its leaderboard (CoreV2 #434). There is no call left to restore. The game's own next
    // step is this delegate: EndMinigame (0x107C820) records every final score, then SetMinigameState(WaitingPostMatch)
    // runs HandleMinigameStateChanged, which broadcasts MinigameEndedDelegate as the game stops being in progress. So the
    // scores the game marked recorded go to the leaderboard here, each once.
    if (Role != ROLE_Authority)
    {
        return;
    }
    AFortMinigameScoreRegistry* Registry = ScoreRegistry;
    if (!Registry)
    {
        return;
    }
    FMinigamePlayerArray& Players = PlayerArray;
    TStructArray<FMinigamePlayerEntry>& Entries = Players.Players;
    for (int32_t Index = 0; Index < Entries.Num(); ++Index)
    {
        FMinigamePlayerEntry& Entry = Entries[Index];
        AFortPlayerState* PlayerState = Entry.PlayerState;
        if (!PlayerState || !Entry.bRecordedScore || !RecordedEntries.emplace(this, static_cast<int32_t>(Entry.ReplicationID)).second)
        {
            continue;
        }
        Registry->RecordScore(PlayerState, Entry.Score);
    }
#endif
}

DEFINE_FUNCTION(AFortMinigameState::execRecordFinalScores)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortMinigameState*>(Context)->RecordFinalScores();
    P_NATIVE_END;
}

void AFortMinigameState::HandleActorSpawned(AActor* Actor)
{
    AFortMinigameState* State = Actor ? Cast<AFortMinigameState>(reinterpret_cast<UObject*>(Actor)) : nullptr;
    if (!State || State->Role != ROLE_Authority || !RecordFinalScoresFunction)
    {
        return;
    }
    FMulticastScriptDelegate& MinigameEnded = State->MinigameEndedDelegate;
    MinigameEnded.AddUniqueDynamic(State, RecordFinalScoresFunction->GetFName());
}

void AFortMinigameState::BindWorld(UWorld* World)
{
#if WITH_SERVER_CODE
    // Only where RecordScore was restored (6.20 and 6.21): the minigame states the world spawns from now on (a playset's
    // minigame mode spawns its state as the playset's level loads, after the match's playlist is in) call
    // RecordFinalScores as each of their games ends.
    if (!World || !RecordFinalScoresFunction || !Natives::AFortMinigameScoreRegistry_RecordScore || BoundWorld.Get() == World)
    {
        return;
    }
    BoundWorld = World;
    RecordedEntries.clear();
    const FDelegateHandle Handle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* Actor) { HandleActorSpawned(Actor); }));
    Log::Info(L"Minigame: the minigame states {} (UWorld::OnActorSpawned, then their MinigameEndedDelegate)",
        Handle.IsValid() ? L"send their players' final scores to their leaderboard" : L"can't be bound: the world's OnActorSpawned wasn't found, so no final score reaches a leaderboard");
#endif
}

void AFortMinigameState::HookNatives()
{
#if WITH_SERVER_CODE
    // The state's MinigameEndedDelegate calls it on the state through the class's FuncMap. Its signature has no parameters.
    // After AFortMinigameScoreRegistry::HookNatives, and only where it restored RecordScore.
    UClass* Class = Natives::AFortMinigameScoreRegistry_RecordScore ? StaticClass() : nullptr;
    const UProperty* Property = Class && Finder::ResolveClassFunctionMap() ? Class->FindPropertyByName(FName(L"MinigameEndedDelegate")) : nullptr;
    const UFunction* Signature = Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
        ? static_cast<const UFunction*>(static_cast<const UMulticastDelegateProperty*>(Property)->SignatureFunction) : nullptr;
    RecordFinalScoresFunction = Signature ? Class->AddNativeFunction(L"RecordFinalScores", &execRecordFinalScores, Signature) : nullptr;
#endif
}
