#include "pch.h"
#include "FortGameState.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/PlayerState.h"
#include "Kismet/KismetMathLibrary.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UI/FrontEnd/FortMissionGenerator.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <map>

void AFortGameState::SpawnMissionManager(const FFortMissionManagerRecord& MissionManagerRecord)
{
    if (Natives::AFortGameState_SpawnMissionManager || Finder::ResolveMissionManagerLive(MissionManagerOffset()))
    {
        Natives::AFortGameState_SpawnMissionManager(this, MissionManagerRecord);
    }
}

bool AFortGameState::Client_RefreshEventCalendar()
{
    UFunction* Function = FindStaticFunction_Client_RefreshEventCalendar();
    if (!Function)
    {
        return false;
    }
    Reflection::FParams Params(Function);
    Params.Call(this);
    return true;
}

const TMap<FString, FString>* AFortGameState::GetMatchOptions() const
{
    const ptrdiff_t Offset = Natives::AFortGameState_MatchOptions;
    return Offset > 0 ? reinterpret_cast<const TMap<FString, FString>*>(reinterpret_cast<const uint8_t*>(this) + Offset) : nullptr;
}

UFortMissionGenerator* AFortGameStateZone::GetMissionGenerator() const
{
    const TAssetSubclassOf<UFortMissionGenerator>& GeneratorClass = MissionGeneratorClass;
    UClass* LoadedClass = GeneratorClass.LoadSynchronous();
    if (!LoadedClass || !LoadedClass->IsChildOf(UFortMissionGenerator::StaticClass()))
    {
        return nullptr;
    }
    return static_cast<UFortMissionGenerator*>(LoadedClass->GetDefaultObject());
}

// Set by AFortGameStateZone::SetDifficultyIncreaseRewardTier (3.0 0xB7E650, intact), which Mission_BluGloPylon calls on
// each deposit and which refuses a tier above the zone's difficulty row's NumDifficultyIncreases. 3.0's own reader
// left is the mission status analytics (0xCA1140); its EndPrimaryMission sender is stripped (the FName is only made,
// 0xFFF10), so Core's sends it.
int32_t AFortGameStateZone::GetDifficultyIncreaseRewardTier() const
{
    return Member_DifficultyIncreaseRewardTier.IsAvailable() ? DifficultyIncreaseRewardTier : 0;
}

bool AFortGameStateZone::GetPlayerStructures(int32_t& OutTotal, int32_t& OutMax) const
{
    if (!Member_TotalPlayerStructures.IsAvailable() || !Member_MaxPlayerStructures.IsAvailable())
    {
        return false;
    }
    OutTotal = TotalPlayerStructures;
    OutMax = MaxPlayerStructures;
    return true;
}

bool AFortGameStateZone::SetMaxPlayerStructures(int32_t NewMax)
{
    if (!Member_MaxPlayerStructures.IsAvailable())
    {
        return false;
    }
    MaxPlayerStructures = NewMax;
    return true;
}

void AFortGameStateZone::SetGameplayState(EFortGameplayState NewGameplayState)
{
    if (Natives::AFortGameStateZone_SetGameplayStateIndex >= 0)
    {
        CallVirtual<void>(this, Natives::AFortGameStateZone_SetGameplayStateIndex, static_cast<uint8_t>(NewGameplayState));
    }
}

bool AFortGameStateZone::OnAbilitySystemInterfaceReadyForPersistentGameplayEffects(IAbilitySystemInterface* AbilitySystemInterface)
{
    if (!AbilitySystemInterface || !Finder::ResolvePersistentGameplayEffectsLive())
    {
        return false;
    }
    Natives::AFortGameStateZone_OnAbilitySystemInterfaceReadyForPersistentGameplayEffects(this, AbilitySystemInterface);
    return true;
}

// Mission Control (5.10's patch notes: the new mission activation and voting replaces the Difficulty Pylon on Retrieve
// the Data and Ride the Lightning; with more than one player, a difficulty increase, and starting before the mission
// ready timer runs out, take a majority vote). The mission (Mission_DeadDrop, with UseNewReadyUp from
// UFortRuntimeOptions::bEnableMissionActivationVote, on by default: 0x12DF540 sets +0x3F4) runs GenericObjective_ReadyUp,
// which spawns BP_ReadyUp_Interact, the console. Interacting opens MissionActivationWidget on the player's client
// (OpenMissionControl, a multicast, which also binds the console's VoteCalled to the game state's
// BindDelegateOnGameplayVoteUpdated for both vote types, on the server too). The widget's UFortMissionActivationInfo
// RequestVote (5.10 0x2E3C120) sends ServerBeginGameplayVote (AutomaticPass when the player is alone), then
// ServerSubmitGameplayVote for a yes, then, alone or with AutomaticPass, ServerEndGameplayVote; the other players' widgets
// send their ServerSubmitGameplayVote. 6.02's RequestVote (0x2FCF9F0) sends ServerBeginGameplayVote with an
// FFortVoteConfig (two options, one vote each) and its ServerSubmitGameplayVote for a yes, and never ends the vote: the
// last voter's vote or VoteDuration does. The controller's _Implementations pass the player state's UniqueId to the game
// state's functions below. On the server the console hears Begin or Update (takes the BluGlo), End with result 1 (starts
// the mission: Global.ActivatePrimary; or raises the difficulty: Mission_BluGloPylon.IncreaseDifficulty, which sets the
// zone's DifficultyIncreaseRewardTier, and the next difficulty vote costs 2, then 3 BluGlo), and BeginLockout (returns
// the BluGlo). OnRep_GameplayVotes (5.10 0xE142E0, 6.02 0xE97120, intact) is what tells the delegates: it compares each
// vote's VoteStatus with its LastVoteStatus, and after a failed End with a lockout also sends BeginLockout.
FVoteData* AFortGameStateZone::FindGameplayVote(EFortVoteType VoteType)
{
    TStructArray<FVoteData>& Votes = GameplayVotesArray;
    for (int32_t Index = 0; Index < Votes.Num(); ++Index)
    {
        if (static_cast<uint8_t>(Votes[Index].VoteType) == static_cast<uint8_t>(VoteType))
        {
            return &Votes[Index];
        }
    }
    return nullptr;
}

int32_t AFortGameStateZone::GetVoteResult(EFortVoteArbitratorType VoteArbitratorType, int32_t NumVoteOptions, const TStructArray<FVoter>& Voters)
{
    if (Voters.Num() == 0)
    {
        return -1;
    }
    if (VoteArbitratorType == EFortVoteArbitratorType::GetAutomaticPassIfAvailable())
    {
        return 1;
    }

    // The voters per decision (-1, not voted, is a decision too: it can be the majority, and then there's no result).
    std::map<int32_t, int32_t> VotesPerDecision;
    for (int32_t Index = 0; Index < Voters.Num(); ++Index)
    {
        ++VotesPerDecision[Voters[Index].VoteDecision];
    }
    if (VoteArbitratorType == EFortVoteArbitratorType::GetMajority())
    {
        int32_t NumMajorities = 0;
        int32_t Result = -1;
        for (const auto& [Decision, Count] : VotesPerDecision)
        {
            if (static_cast<int64_t>(Count) * NumVoteOptions > Voters.Num())
            {
                ++NumMajorities;
                Result = Decision;
            }
        }
        return NumMajorities == 1 ? Result : -1;
    }
    if (VoteArbitratorType == EFortVoteArbitratorType::GetUnanimous() && VotesPerDecision.size() == 1)
    {
        return Voters[0].VoteDecision;
    }
    return -1;
}

void AFortGameStateZone::GameplayVotesChanged()
{
    if (UFunction* OnRep = FindStaticFunction_OnRep_GameplayVotes())
    {
        Reflection::FParams Params(OnRep);
        Params.Call(this);
    }
    if (Natives::AActor_ForceNetUpdateIndex >= 0)
    {
        ForceNetUpdate();
    }
}

FVoteData* AFortGameStateZone::AddGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId)
{
    if (FindGameplayVote(VoteType))
    {
        Log::Info(L"Mission Control: a vote for {} is already in progress or has recently failed", VoteType.ToString());
        return nullptr;
    }

    UScriptStruct* const NetIdStruct = FUniqueNetIdRepl::StaticStruct();
    FVoteData& NewVote = GameplayVotesArray.AddDefaulted();
    NewVote.VoteType = VoteType;
    NetIdStruct->CopyScriptStruct(&NewVote.GetMember_VoteInstigatorNetId(), &PlayerNetId);
    NewVote.VoteStartTime = UKismetMathLibrary::UtcNow();
    NewVote.VoteStatus = EFortVoteStatus::GetBegin();
    NewVote.ResetLastVoteStatus();

    // Every player in the zone votes, none of them yet.
    for (APlayerState* Player : PlayerArray)
    {
        if (!Player || Player->bIsABot || !Player->GetMember_UniqueId().IsValid())
        {
            continue;
        }
        FVoter& Voter = NewVote.GetMember_Voters().AddDefaulted();
        Voter.VoteDecision = -1;
        Voter.LastVoteDecision = -1;
        NetIdStruct->CopyScriptStruct(&Voter.GetMember_NetId(), &Player->GetMember_UniqueId());
    }
    return &NewVote;
}

void AFortGameStateZone::StartGameplayVote(EFortVoteType VoteType)
{
    FVoteData* Vote = FindGameplayVote(VoteType);
    UWorld* World = Vote ? GetWorld() : nullptr;
    if (!Vote)
    {
        return;
    }
    Log::Info(L"Mission Control: {} starts a {} vote ({} voters, {} for {:.0f}s, lockout {:.0f}s, {} options, {} votes each)",
        Vote->GetMember_VoteInstigatorNetId().ToString().ToStdString(), VoteType.ToString(), Vote->GetMember_Voters().Num(),
        Vote->GetVoteArbitratorType().ToString(), Vote->GetVoteDuration(), Vote->GetFailedVoteLockOutDuration(), Vote->GetNumVoteOptions(),
        Vote->GetMaxVotesAllowedPerPlayer());

    // When the time runs out the vote ends with what the voters have decided by then.
    if (World && Vote->GetVoteDuration() > 0.0f)
    {
        const TWeakObjectPtr<AFortGameStateZone> WeakThis(this);
        World->GetTimerManager().SetTimer(Vote->GetMember_EndVoteTimerHandle(), FTimerDelegate::CreateLambda([WeakThis, VoteType]()
        {
            AFortGameStateZone* GameState = WeakThis.Get();
            FVoteData* Running = GameState ? GameState->FindGameplayVote(VoteType) : nullptr;
            if (!Running || Running->VoteStatus == EFortVoteStatus::GetEnd())
            {
                return;
            }
            Running->GetMember_EndVoteTimerHandle().Invalidate();
            Log::Info(L"Mission Control: the {} vote's time has run out", VoteType.ToString());
            GameState->ServerEndGameplayVote(VoteType, Running->GetMember_VoteInstigatorNetId());
        }), Vote->GetVoteDuration(), false);
    }
    GameplayVotesChanged();
}

void AFortGameStateZone::ServerBeginGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId, float VoteDuration, float FailedVoteLockOutDuration, EFortVoteArbitratorType VoteArbitratorType)
{
#if WITH_SERVER_CODE
    if (static_cast<ENetRole>(static_cast<uint8_t>(Role)) != ROLE_Authority || !FVoteData::HasVoteSettings())
    {
        return;
    }
    FVoteData* NewVote = AddGameplayVote(VoteType, PlayerNetId);
    if (!NewVote)
    {
        return;
    }
    NewVote->VoteDuration = VoteDuration;
    NewVote->FailedVoteLockOutDuration = FailedVoteLockOutDuration;
    NewVote->VoteArbitratorType = VoteArbitratorType;
    StartGameplayVote(VoteType);
#endif
}

void AFortGameStateZone::ServerBeginGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId, const FFortVoteConfig& VoteConfig)
{
#if WITH_SERVER_CODE
    if (static_cast<ENetRole>(static_cast<uint8_t>(Role)) != ROLE_Authority || !FVoteData::HasVoteConfig())
    {
        return;
    }
    FVoteData* NewVote = AddGameplayVote(VoteType, PlayerNetId);
    if (!NewVote)
    {
        return;
    }
    FFortVoteConfig::StaticStruct()->CopyScriptStruct(&NewVote->GetMember_VoteConfig(), &VoteConfig);
    NewVote->NumVotersWithMaxVotes = 0;
    StartGameplayVote(VoteType);
#endif
}

void AFortGameStateZone::ServerSubmitGameplayVote(EFortVoteType VoteType, int32_t VoteDecision, const FUniqueNetIdRepl& PlayerNetId)
{
#if WITH_SERVER_CODE
    if (static_cast<ENetRole>(static_cast<uint8_t>(Role)) != ROLE_Authority)
    {
        return;
    }
    FVoteData* Vote = FindGameplayVote(VoteType);
    if (!Vote || Vote->VoteStatus == EFortVoteStatus::GetEnd())
    {
        Log::Info(L"Mission Control: a vote for {} is not in progress", VoteType.ToString());
        return;
    }

    // The player's vote; a player who came in after the vote began votes too.
    const std::wstring PlayerId = PlayerNetId.ToString().ToStdString();
    TStructArray<FVoter>& Voters = Vote->GetMember_Voters();
    FVoter* Voter = nullptr;
    for (int32_t Index = 0; Index < Voters.Num() && !Voter; ++Index)
    {
        Voter = Voters[Index].GetMember_NetId().ToString().ToStdString() == PlayerId ? &Voters[Index] : nullptr;
    }
    if (!Voter && PlayerNetId.IsValid())
    {
        Voter = &Voters.AddDefaulted();
        Voter->LastVoteDecision = -1;
        FUniqueNetIdRepl::StaticStruct()->CopyScriptStruct(&Voter->GetMember_NetId(), &PlayerNetId);
    }
    if (!Voter)
    {
        return;
    }

    // 5.30+: each voter casts up to the vote's MaxVotesAllowedPerPlayer votes, and the vote is decided once every voter
    // has cast them all (NumVotersWithMaxVotes). 5.0-5.21's voters have one vote, which they may change until everyone
    // has voted.
    const bool bCountsVotes = FVoteData::HasVoteConfig();
    const int32_t MaxVotes = Vote->GetMaxVotesAllowedPerPlayer();
    if (bCountsVotes && MaxVotes > 0 && Voter->NumVotesCast >= MaxVotes)
    {
        Log::Info(L"Mission Control: {} has cast all {} of their votes on {}", PlayerId, MaxVotes, VoteType.ToString());
        return;
    }
    Voter->VoteDecision = VoteDecision;
    if (bCountsVotes)
    {
        Voter->NumVotesCast = Voter->NumVotesCast + 1;
        if (Voter->NumVotesCast == MaxVotes)
        {
            Vote->NumVotersWithMaxVotes = Vote->NumVotersWithMaxVotes + 1;
        }
    }
    Vote->VoteStatus = EFortVoteStatus::GetUpdate();

    int32_t NumVoted = 0;
    for (int32_t Index = 0; Index < Voters.Num(); ++Index)
    {
        NumVoted += Voters[Index].VoteDecision != -1 ? 1 : 0;
    }
    const int32_t NumDone = bCountsVotes ? Vote->NumVotersWithMaxVotes : NumVoted;
    const int32_t NumVoters = Voters.Num();
    Log::Info(L"Mission Control: {} votes {} on {} ({} of {} voted)", PlayerId, VoteDecision, VoteType.ToString(), NumVoted, NumVoters);
    GameplayVotesChanged();

    // Once everyone has voted the vote is decided: it ends without waiting out its time.
    if (NumDone >= NumVoters)
    {
        if (FVoteData* Decided = FindGameplayVote(VoteType))
        {
            ServerEndGameplayVote(VoteType, Decided->GetMember_VoteInstigatorNetId());
        }
    }
#endif
}

void AFortGameStateZone::ServerEndGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId)
{
#if WITH_SERVER_CODE
    if (static_cast<ENetRole>(static_cast<uint8_t>(Role)) != ROLE_Authority)
    {
        return;
    }
    FVoteData* Vote = FindGameplayVote(VoteType);
    if (!Vote || Vote->VoteStatus == EFortVoteStatus::GetEnd())
    {
        Log::Info(L"Mission Control: a vote for {} is not in progress or has already ended", VoteType.ToString());
        return;
    }
    // Only the player who called the vote ends it early (5.10's widget does when they're alone, or for an automatic
    // pass); the others' votes end it, or its time does.
    if (PlayerNetId.ToString().ToStdString() != Vote->GetMember_VoteInstigatorNetId().ToString().ToStdString())
    {
        Log::Warning(L"Mission Control: {} can't end the {} vote {} called", PlayerNetId.ToString().ToStdString(), VoteType.ToString(),
            Vote->GetMember_VoteInstigatorNetId().ToString().ToStdString());
        return;
    }

    UWorld* World = GetWorld();
    if (World && Vote->GetMember_EndVoteTimerHandle().IsValid())
    {
        World->GetTimerManager().ClearTimer(Vote->GetMember_EndVoteTimerHandle());
    }
    Vote->VoteEndTime = UKismetMathLibrary::UtcNow();
    Vote->VoteStatus = EFortVoteStatus::GetEnd();
    const int32_t VoteResult = GetVoteResult(Vote->GetVoteArbitratorType(), Vote->GetNumVoteOptions(), Vote->GetMember_Voters());
    Log::Info(L"Mission Control: the {} vote ends with result {} ({})", VoteType.ToString(), VoteResult, VoteResult == 1 ? L"passed" : L"failed");
    GameplayVotesChanged();
    ScheduleRemoveGameplayVote(VoteType);
#endif
}

void AFortGameStateZone::ScheduleRemoveGameplayVote(EFortVoteType VoteType)
{
#if WITH_SERVER_CODE
    // The delegates OnRep_GameplayVotes told may have changed the array: find the vote again.
    FVoteData* Vote = FindGameplayVote(VoteType);
    UWorld* World = Vote && Vote->VoteStatus == EFortVoteStatus::GetEnd() ? GetWorld() : nullptr;
    if (!World)
    {
        return;
    }
    // A moment, so that every client sees the vote end (its OnRep tells the widget the result) before it goes and the
    // next vote of its type can begin; a failed one with a lockout stays locked that long.
    constexpr float EndedVoteSeconds = 2.0f;
    const bool bFailed = GetVoteResult(Vote->GetVoteArbitratorType(), Vote->GetNumVoteOptions(), Vote->GetMember_Voters()) <= 0;
    const bool bLockout = bFailed && Vote->GetFailedVoteLockOutDuration() > 0.0f;
    const float Remain = bLockout ? std::max(Vote->GetFailedVoteLockOutDuration(), EndedVoteSeconds) : EndedVoteSeconds;

    const TWeakObjectPtr<AFortGameStateZone> WeakThis(this);
    World->GetTimerManager().SetTimer(Vote->GetMember_EndVoteTimerHandle(), FTimerDelegate::CreateLambda([WeakThis, VoteType, bLockout]()
    {
        AFortGameStateZone* GameState = WeakThis.Get();
        if (!GameState)
        {
            return;
        }
        TStructArray<FVoteData>& Votes = GameState->GameplayVotesArray;
        for (int32_t Index = Votes.Num() - 1; Index >= 0; --Index)
        {
            if (static_cast<uint8_t>(Votes[Index].VoteType) == static_cast<uint8_t>(VoteType) && Votes[Index].VoteStatus == EFortVoteStatus::GetEnd())
            {
                Votes.RemoveAt(Index);
            }
        }
        GameState->GameplayVotesChanged();
        if (!bLockout)
        {
            return;
        }
        Log::Info(L"Mission Control: the {} vote's lockout is over", VoteType.ToString());
        if (UFunction* Notify = FindStaticFunction_NotifyEndFailedGameplayVoteLockout())
        {
            Reflection::FParams Params(Notify);
            Params.Set(L"VoteType", VoteType);
            Params.Call(GameState);
        }
    }), Remain, false);
#endif
}

bool AFortGameStateZone::AreGameplayVotesStripped()
{
    // The votes' functions share one #if: the game state's ServerSubmitGameplayVote is the shared empty function (5.10
    // CL-4240749: its exec, 0x13ACC80, calls nullsub_2; 6.02 CL-4461277: 0x147AB60 calls nullsub_3) wherever the server
    // code is stripped.
    UFunction* const Submit = FindStaticFunction_ServerSubmitGameplayVote();
    const uintptr_t Exec = Submit ? reinterpret_cast<uintptr_t>(Submit->GetNativeFunc()) : 0;
    const uintptr_t ExecEnd = Exec ? Finder::FunctionEnd(Exec) : 0;
    if (!ExecEnd || !FVoteData::IsAvailable() || !FVoter::StaticStruct() || !FindStaticFunction_OnRep_GameplayVotes())
    {
        return false;
    }
    for (const auto& [Site, Target] : Finder::DirectCalls(Exec, ExecEnd))
    {
        if (Finder::IsStubFunction(Target))
        {
            return true;
        }
    }
    return false;
}

ptrdiff_t AFortGameState::MissionManagerOffset()
{
    return Member_MissionManager.GetOffsetChecked();
}
