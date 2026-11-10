#include "pch.h"
#include "FortGameMode.h"
#include "Containers/ScriptArray.h"
#include "CoreGlobals.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Athena/FortGameModeAthena.h"
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
#include "Online/FortMcpProfileCampaign.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "Templates/SharedPointer.h"
#include "UObject/CoreOnline.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

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
        const TSharedPtr<const FUniqueNetId> SessionId = GameInstance ? GameInstance->GetGameSessionId() : TSharedPtr<const FUniqueNetId>();
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

bool AFortGameMode::OverridesQuickBars()
{
    return Member_bOverrideQuickBars.IsAvailable() && bOverrideQuickBars;
}

UFortSharedMissionLists* AFortGameMode::GetSharedMissionLists()
{
    if (!SharedMissionLists)
    {
        UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
        UClass* SharedMissionListsClass = GameData ? GameData->GetSharedMissionListsClass() : nullptr;
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

void AFortGameMode::GetPoiTagsFor(AActor* Actor, FGameplayTagContainer& OutTags)
{
#if WITH_SERVER_CODE
    if (Actor && Finder::ResolveGetPoiTagsForLive())
    {
        CallVirtual<void>(this, Natives::AFortGameMode_GetPoiTagsForIndex, Actor, &OutTags);
    }
#endif
}

void AFortGameMode::SetBadgeScores()
{
    if (Natives::AFortGameMode_SetBadgeScores || Finder::ResolveBadgeScoresLive())
    {
        Natives::AFortGameMode_SetBadgeScores(this);
    }
}

void AFortGameMode::SendEarnScore(TArray<FAccountIdAndScore>* OutPlayersAndScores)
{
    const bool bScoresForEndMatch = Natives::AFortGameMode_GetPlayersAndScores && Natives::AFortGameMode_GrantScoreRewardIndex >= 0;
    if (Natives::AFortGameMode_SendEarnScoreIndex < 0 && !bScoresForEndMatch)
    {
        return;
    }

#if WITH_SERVER_CODE
    // From 1.7.2 every score grant (GrantScoreReward, 1.8 0x58A960) first gives each player in the world a placeholder
    // ExperienceDelta (-1s, NotFinal or Final), and each earnScore notification fills the player's first unfilled one
    // (0x840910); the results screen waits until the last delta is a filled Final one (IsDataFinalized, 0x2F1A940).
    // AFortGameMode::OnEndOfDay (slot 0x137, bound to the time of day manager's end of day by SetTimeOfDayManager,
    // 0x5E8F50) grants non-final, but its per-player check is the same stripped stub (32 C0 C3) as SendEarnScore's and
    // the grant's EarnScore send (the call at its end, 0x58AD88) is an empty function, so those placeholders are never
    // filled. A mission that ran past the end of a day left each player a NotFinal placeholder in front of the Final one,
    // and the zone end's earnScore filled that instead. The player the EarnScore went through also gets an empty
    // earnScore in their EndPrimaryMission response (Core-Backend), which filled their Final delta, but every other
    // player got only theirs and their results screen never showed (issue #121). Nothing will fill those placeholders,
    // so take them out first and the zone end's notification fills the Final one.
    UWorld* World = GetWorld();
    for (TObjectIterator<AFortPlayerController> It; It; ++It)
    {
        APlayerState* PlayerState = It->GetWorld() == World ? static_cast<APlayerState*>(It->PlayerState) : nullptr;
        AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(PlayerState));
        if (const int32_t Removed = FortPlayerState ? FortPlayerState->RemoveUnscoredExperienceDeltas() : 0)
        {
            Log::Info(L"SendEarnScore: took out {} experience delta(s) of {} that no EarnScore was sent for", Removed, It->GetName().ToStdString());
        }
    }
#endif

    if (bScoresForEndMatch)
    {
        // 5.21's SendEarnScore(bIsFinal) (slot 0x140, 0xE07650) has the scores built (0xDF6220) and granted (slot 0x13A,
        // the ExperienceDelta placeholders the earnScore notifications fill), and sends nothing: each player's score
        // goes in their EndMatch entry (AFortMissionManager::OnZoneEnd).
        TArray<FAccountIdAndScore> PlayersAndScores;
        Natives::AFortGameMode_GetPlayersAndScores(this, reinterpret_cast<FScriptArray*>(&PlayersAndScores));
        CallVirtual<void>(this, Natives::AFortGameMode_GrantScoreRewardIndex, static_cast<const TArray<FAccountIdAndScore>*>(&PlayersAndScores), true);
        for (const FAccountIdAndScore& Entry : PlayersAndScores)
        {
            Log::Info(L"SendEarnScore: {} total score {}, critical match bonus {} (for EndMatch)", Entry.AccountId.ToStdString(), Entry.TotalScore, Entry.bCriticalMatchBonus);
        }
        if (OutPlayersAndScores)
        {
            *OutPlayersAndScores = std::move(PlayersAndScores);
        }
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
    UFortMcpProfileCampaign* AccountProfile = PlayerInfo ? static_cast<UFortMcpProfileCampaign*>(PlayerInfo->AccountProfile) : nullptr;
    if (!AccountProfile)
    {
        Log::Error(L"EarnScore: {} has no account profile, {} players get no score", SendingPlayer ? SendingPlayer->GetName().ToStdString() : std::wstring(L"(none)"), PlayersAndScores.Num());
        return;
    }

    if (!AccountProfile->EarnScore(GetGameSessionId(SendingPlayer->GetWorld()), PlayersAndScores))
    {
        Log::Error(L"EarnScore: {} has no EarnScore command, {} players get no score", AccountProfile->GetClass()->GetName().ToStdString(), PlayersAndScores.Num());
        return;
    }
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
    if (UFortMcpProfileCampaign::HasEndMatch())
    {
        // 5.21: SendEarnScore's scores are built by a callee, whose gate and badges score are the same stripped stubs.
        if (Finder::ResolveMatchEndScoresLive(Sites))
        {
            Hooks::RedirectCall(L"AFortGameMode::SendEarnScore scores -> ShouldPlayerEarnScore", Sites.GateCall, reinterpret_cast<void*>(&ShouldPlayerEarnScore));
            Hooks::RedirectCall(L"AFortGameMode::SendEarnScore scores -> GetPlayerBadgesScore", Sites.ScoreCall, reinterpret_cast<void*>(&GetPlayerBadgesScore));
        }
        return;
    }
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
    // GetGameSessionClass is virtual. AFortGameModeAthena's override (5.0 0xBB7ED0) is stripped to its Super call, which
    // lands here, so the override is dispatched to from here.
    if (const AFortGameModeAthena* Athena = Cast<AFortGameModeAthena>(This))
    {
        *Result = Athena->GetGameSessionClass();
        return Result;
    }
    *Result = This->GetGameSessionClass();
    return Result;
}

UFortGameDeathPenalty* AFortGameMode::GetDeathPenaltyData()
{
    // 4.0 split the member: DefaultDeathPenaltyData holds the path and DeathPenaltyData the loaded asset. Before 4.0,
    // DeathPenaltyData was the path itself.
    if (Member_DeathPenaltyData.IsAvailable())
    {
        if (UFortGameDeathPenalty* DeathPenalty = DeathPenaltyData)
        {
            return DeathPenalty;
        }
        return Member_DefaultDeathPenaltyData.IsAvailable() ? DefaultDeathPenaltyData.LoadSynchronous() : nullptr;
    }
    if (Member_DeathPenaltyDataPath.IsAvailable())
    {
        return Reflection::AccessMember<TAssetPtr<UFortGameDeathPenalty>>(this, Member_DeathPenaltyDataPath).LoadSynchronous();
    }
    return nullptr;
}
