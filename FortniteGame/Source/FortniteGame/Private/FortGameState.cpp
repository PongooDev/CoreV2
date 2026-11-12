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
#include "UI/FrontEnd/FortMissionGenerator.h"

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
// RequestVote (0x2E3C120) sends ServerBeginGameplayVote (AutomaticPass when the player is alone), then
// ServerSubmitGameplayVote for a yes, then, alone or with AutomaticPass, ServerEndGameplayVote; the other players' widgets
// send their ServerSubmitGameplayVote. The controller's _Implementations pass the player state's UniqueId to the game
// state's functions below. On the server the console hears Begin or Update (takes the BluGlo), End with result 1 (starts
// the mission: Global.ActivatePrimary; or raises the difficulty: Mission_BluGloPylon.IncreaseDifficulty), and
// BeginLockout (returns the BluGlo). OnRep_GameplayVotes (0xE142E0, intact) is what tells the delegates: it compares each
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

int32_t AFortGameStateZone::GetVoteResult(EFortVoteArbitratorType VoteArbitratorType, const TStructArray<FVoter>& Voters)
{
    if (Voters.Num() == 0)
    {
        return -1;
    }
    if (VoteArbitratorType == EFortVoteArbitratorType::GetAutomaticPass())
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
        for (const auto& [Decision, Count] : VotesPerDecision)
        {
            if (2 * Count > Voters.Num())
            {
                return Decision;
            }
        }
        return -1;
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

void AFortGameStateZone::ServerBeginGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId, float VoteDuration, float FailedVoteLockOutDuration, EFortVoteArbitratorType VoteArbitratorType)
{
#if WITH_SERVER_CODE
    if (static_cast<ENetRole>(static_cast<uint8_t>(Role)) != ROLE_Authority)
    {
        return;
    }
    if (FindGameplayVote(VoteType))
    {
        Log::Info(L"Mission Control: a vote for {} is already in progress or has recently failed", VoteType.ToString());
        return;
    }

    UScriptStruct* const NetIdStruct = FUniqueNetIdRepl::StaticStruct();
    FVoteData& NewVote = GameplayVotesArray.AddDefaulted();
    NewVote.VoteType = VoteType;
    NetIdStruct->CopyScriptStruct(&NewVote.GetMember_VoteInstigatorNetId(), &PlayerNetId);
    NewVote.VoteStartTime = UKismetMathLibrary::UtcNow();
    NewVote.VoteDuration = VoteDuration;
    NewVote.FailedVoteLockOutDuration = FailedVoteLockOutDuration;
    NewVote.VoteArbitratorType = VoteArbitratorType;
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

    Log::Info(L"Mission Control: {} starts a {} vote ({} voters, {} for {:.0f}s, lockout {:.0f}s)", PlayerNetId.ToString().ToStdString(), VoteType.ToString(),
        NewVote.GetMember_Voters().Num(), VoteArbitratorType.ToString(), VoteDuration, FailedVoteLockOutDuration);
    GameplayVotesChanged();
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
    Voter->VoteDecision = VoteDecision;
    Vote->VoteStatus = EFortVoteStatus::GetUpdate();

    int32_t NumVoted = 0;
    for (int32_t Index = 0; Index < Voters.Num(); ++Index)
    {
        NumVoted += Voters[Index].VoteDecision != -1 ? 1 : 0;
    }
    Log::Info(L"Mission Control: {} votes {} on {} ({} of {} voted)", PlayerId, VoteDecision, VoteType.ToString(), NumVoted, Voters.Num());
    GameplayVotesChanged();

    // Once everyone has voted the vote is decided: it ends without waiting out its time.
    if (NumVoted == Voters.Num())
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
    // Only the player who called the vote ends it early (their widget does when they're alone, or for an automatic
    // pass); the others' votes end it, or its time does.
    if (PlayerNetId.ToString().ToStdString() != Vote->GetMember_VoteInstigatorNetId().ToString().ToStdString())
    {
        Log::Warning(L"Mission Control: {} can't end the {} vote {} called", PlayerNetId.ToString().ToStdString(), VoteType.ToString(),
            Vote->GetMember_VoteInstigatorNetId().ToString().ToStdString());
        return;
    }

    Vote->VoteEndTime = UKismetMathLibrary::UtcNow();
    Vote->VoteStatus = EFortVoteStatus::GetEnd();
    const int32_t VoteResult = GetVoteResult(Vote->VoteArbitratorType, Vote->GetMember_Voters());
    Log::Info(L"Mission Control: the {} vote ends with result {} ({})", VoteType.ToString(), VoteResult, VoteResult == 1 ? L"passed" : L"failed");
    GameplayVotesChanged();
#endif
}

bool AFortGameStateZone::AreGameplayVotesStripped()
{
    // The votes' functions share one #if: the game state's ServerSubmitGameplayVote is the shared empty function (5.10
    // CL-4240749: its exec, 0x13ACC80, calls nullsub_2) wherever the server code is stripped.
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

void AFortGameStateZone::TickGameplayVotes()
{
#if WITH_SERVER_CODE
    UWorld* World = Natives::GWorld ? static_cast<UWorld*>(GWorld) : nullptr;
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState || GameState->GameplayVotesArray.Num() == 0)
    {
        return;
    }

    // An ended vote stays in the array a moment, so that every client sees it end (its OnRep tells the widget the
    // result) before it goes and the next vote of its type can begin; a failed one with a lockout stays locked that long.
    constexpr double EndedVoteSeconds = 2.0;
    const FDateTime Now = UKismetMathLibrary::UtcNow();
    TStructArray<FVoteData>& Votes = GameState->GameplayVotesArray;
    for (int32_t Index = Votes.Num() - 1; Index >= 0; --Index)
    {
        FVoteData& Vote = Votes[Index];
        const EFortVoteType VoteType = Vote.VoteType;
        if (Vote.VoteStatus != EFortVoteStatus::GetEnd())
        {
            if (Vote.VoteDuration > 0.0f && Now.SecondsSince(Vote.VoteStartTime) >= Vote.VoteDuration)
            {
                GameState->ServerEndGameplayVote(VoteType, Vote.GetMember_VoteInstigatorNetId());
            }
            continue;
        }

        const bool bFailed = GetVoteResult(Vote.VoteArbitratorType, Vote.GetMember_Voters()) <= 0;
        const bool bLockout = bFailed && Vote.FailedVoteLockOutDuration > 0.0f;
        const double Remain = bLockout ? std::max<double>(Vote.FailedVoteLockOutDuration, EndedVoteSeconds) : EndedVoteSeconds;
        if (Now.SecondsSince(Vote.VoteEndTime) < Remain)
        {
            continue;
        }
        Votes.RemoveAt(Index);
        GameState->GameplayVotesChanged();
        if (bLockout)
        {
            Log::Info(L"Mission Control: the {} vote's lockout is over", VoteType.ToString());
            if (UFunction* Notify = FindStaticFunction_NotifyEndFailedGameplayVoteLockout())
            {
                Reflection::FParams Params(Notify);
                Params.Set(L"VoteType", VoteType);
                Params.Call(GameState);
            }
        }
    }
#endif
}

ptrdiff_t AFortGameState::MissionManagerOffset()
{
    return Member_MissionManager.GetOffsetChecked();
}
