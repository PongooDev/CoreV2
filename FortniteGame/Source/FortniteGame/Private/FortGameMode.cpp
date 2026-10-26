#include "pch.h"
#include "FortGameMode.h"
#include "Containers/ScriptArray.h"
#include "CoreGlobals.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Log.h"
#include "Missions/FortSharedMissionLists.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Online/FortGameSessionDedicated.h"
#include "Online/FortMcpProfileAccount.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "Templates/SharedPointer.h"
#include "UObject/CoreOnline.h"
#include "UObject/UObjectGlobals.h"

#include <utility>

namespace
{
    struct FPlayerControllerArrayView
    {
        AFortPlayerController** Data;
        int32_t Num;
    };

    FString GetGameSessionId(UWorld* World)
    {
        UGameInstance* GameInstance = World ? static_cast<UGameInstance*>(World->OwningGameInstance) : nullptr;
        TSharedPtr<const FUniqueNetId> SessionId;
        if (GameInstance && Natives::UGameInstance_GetGameSessionId)
        {
            Natives::UGameInstance_GetGameSessionId(GameInstance, &SessionId);
        }
        return SessionId.IsValid() && SessionId->IsValid() ? SessionId->ToString() : FString();
    }
}

TSubclassOf<AGameSession> AFortGameMode::GetGameSessionClass() const
{
    if (IsRunningDedicatedServer())
    {
        return AFortGameSessionDedicated::StaticClass();
    }

    return AFortGameSession::StaticClass();
}

APawn* AFortGameMode::SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot)
{
    return Natives::AFortGameMode_SpawnDefaultPawnFor(this, NewPlayer, StartSpot);
}

void AFortGameMode::FinishWorldInitialization(AFortWorldManager* WorldManager)
{
    Natives::AFortGameMode_FinishWorldInitialization(this, WorldManager);
}

UFortSharedMissionLists* AFortGameMode::GetSharedMissionLists()
{
    if (!SharedMissionLists)
    {
        UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
        UClass* SharedMissionListsClass = GameData ? static_cast<UClass*>(GameData->SharedMissionListsClass) : nullptr;
        if (SharedMissionListsClass && SharedMissionListsClass->IsChildOf(UFortSharedMissionLists::StaticClass()))
        {
            SharedMissionLists = static_cast<UFortSharedMissionLists*>(SharedMissionListsClass->GetDefaultObject());
        }
        else
        {
            SharedMissionLists = NewObject<UFortSharedMissionLists>(StaticFindObject(nullptr, nullptr, L"/Engine/Transient"), UFortSharedMissionLists::StaticClass());
            Log::Warning(L"nullptr Shared Missions Lists class specified by Game Data; created default Shared Mission Lists anyway, but some settings may be missing.");
        }
    }
    return SharedMissionLists;
}

void AFortGameMode::SetBadgeScores()
{
    if (Natives::AFortGameMode_SetBadgeScores || Finder::ResolveBadgeScoresLive())
    {
        Natives::AFortGameMode_SetBadgeScores(this);
    }
}

void AFortGameMode::SendEarnScore()
{
    if (Natives::AFortGameMode_SendEarnScoreIndex < 0)
    {
        return;
    }

    PendingPlayersAndScores = nullptr;
    // bIsFinal: 1.7.2's SendEarnScore(bool) only builds the EarnScore context when it is set; 1.1 and 1.2 ignore it.
    CallVirtual<void>(this, Natives::AFortGameMode_SendEarnScoreIndex, true);
    PendingPlayersAndScores = nullptr;
}

bool AFortGameMode::ShouldPlayerEarnScore(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    APlayerState* PlayerState = PlayerController ? static_cast<APlayerState*>(PlayerController->PlayerState) : nullptr;
    UFortRegisteredPlayerInfo* PlayerInfo = PlayerController ? static_cast<UFortRegisteredPlayerInfo*>(PlayerController->MyPlayerInfo) : nullptr;
    return Cast<AFortPlayerState>(reinterpret_cast<UObject*>(PlayerState)) && PlayerState->UniqueId.IsValid() && PlayerInfo && PlayerInfo->AccountProfile;
#else
    return false;
#endif
}

int32_t AFortGameMode::GetPlayerBadgesScore(AFortPlayerController* PlayerController)
{
#if WITH_SERVER_CODE
    AFortPlayerState* PlayerState = PlayerController ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState))) : nullptr;
    return PlayerState ? PlayerState->GetStat(EFortReplicatedStat::GetBadgesScore(), false) : 0;
#else
    return 0;
#endif
}

void AFortGameMode::SendEarnScoreCommand(AFortPlayerController* SendingPlayer, const TArray<FAccountIdAndScore>& PlayersAndScores)
{
#if WITH_SERVER_CODE
    UFortRegisteredPlayerInfo* PlayerInfo = SendingPlayer ? static_cast<UFortRegisteredPlayerInfo*>(SendingPlayer->MyPlayerInfo) : nullptr;
    UFortMcpProfileAccount* AccountProfile = PlayerInfo ? static_cast<UFortMcpProfileAccount*>(PlayerInfo->AccountProfile) : nullptr;
    if (!AccountProfile)
    {
        Log::Error(L"EarnScore: {} has no account profile, {} players get no score", SendingPlayer ? SendingPlayer->GetName().ToStdString() : std::wstring(L"(none)"), PlayersAndScores.Num());
        return;
    }

    AccountProfile->EarnScore(GetGameSessionId(SendingPlayer->GetWorld()), PlayersAndScores);
    for (const FAccountIdAndScore& Entry : PlayersAndScores)
    {
        Log::Info(L"EarnScore: {} total score {}, critical match bonus {}", Entry.AccountId.ToStdString(), Entry.TotalScore, Entry.bCriticalMatchBonus);
    }
#endif
}

void AFortGameMode::Hook()
{
    Hooks::Detour(L"AFortGameMode::GetGameSessionClass", reinterpret_cast<void*>(Natives::AFortGameMode_GetGameSessionClass), &GetGameSessionClassHK, nullptr);
}

void AFortGameMode::HookCallSites()
{
    Finder::FEarnScoreCallSites Sites;
    if (!Finder::ResolveEarnScoreLive(AFortPlayerController::CriticalMatchBonusOffset(), Sites))
    {
        return;
    }

    Hooks::RedirectCall(L"AFortGameMode::SendEarnScore -> ShouldPlayerEarnScore", Sites.GateCall, reinterpret_cast<void*>(&ShouldPlayerEarnScore));
    Hooks::RedirectCall(L"AFortGameMode::SendEarnScore -> GetPlayerBadgesScore", Sites.ScoreCall, reinterpret_cast<void*>(&GetPlayerBadgesScore));
    Hooks::RedirectCall(L"AFortGameMode::SendEarnScore -> PlayersAndScores grow", Sites.GrowCall, reinterpret_cast<void*>(&ResizeGrowPlayersAndScoresCall));
    Hooks::RedirectCall(L"AFortGameMode::SendEarnScore -> EarnScore context", Sites.ContextCall, reinterpret_cast<void*>(&MakeEarnScoreContextCall));
}

void AFortGameMode::ResizeGrowPlayersAndScoresCall(FScriptArray* PlayersAndScores, int32_t OldNum)
{
    Natives::TArray_FAccountIdAndScore_ResizeGrow(PlayersAndScores, OldNum);
    PendingPlayersAndScores = PlayersAndScores;
}

void* AFortGameMode::MakeEarnScoreContextCall(void* Result, void* Players)
{
    void* const Context = Natives::AFortGameMode_MakeEarnScoreContext(Result, Players);

#if WITH_SERVER_CODE
    const FPlayerControllerArrayView* View = static_cast<const FPlayerControllerArrayView*>(Players);
    FScriptArray* const PlayersAndScores = std::exchange(PendingPlayersAndScores, nullptr);
    if (View && View->Num > 0 && View->Data && View->Data[0] && PlayersAndScores)
    {
        SendEarnScoreCommand(View->Data[0], *reinterpret_cast<const TArray<FAccountIdAndScore>*>(PlayersAndScores));
    }
#endif
    return Context;
}

TSubclassOf<AGameSession>* AFortGameMode::GetGameSessionClassHK(const AFortGameMode* This, TSubclassOf<AGameSession>* Result)
{
    *Result = This->GetGameSessionClass();
    return Result;
}
