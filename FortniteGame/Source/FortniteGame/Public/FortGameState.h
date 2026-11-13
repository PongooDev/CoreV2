#pragma once

#include "Containers/Map.h"
#include "Containers/UnrealString.h"
#include "Engine/EngineTypes.h"
#include "GameFramework/GameState.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Misc/DateTime.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"

class AFortMissionManager;
class AFortVisibilityManager;
class IAbilitySystemInterface;
class AFortWorldManager;
class UBuildingStructuralSupportSystem;
class UFortMissionGenerator;
class UFortZoneTheme;
struct FFortMissionManagerRecord;

class EFortGameplayState
{
    DefineUnrealEnum(EFortGameplayState, FortniteGame)
    DefineUEnumValue(NormalGameplay)
    DefineUEnumValue(WaitingToStart)
    DefineUEnumValue(EndOfZone)
    DefineUEnumValue(EnteringZone)
    DefineUEnumValue(LeavingZone)
    DefineUEnumValue(Invalid)
};

// Mission Control's votes (5.10: "a new system for mission activation and voting", on Retrieve the Data and Ride the
// Lightning): what a vote decides, how it is decided, and where it is.
class EFortVoteType
{
    DefineUnrealEnum(EFortVoteType, FortniteGame)
    DefineUEnumValue(SurvivalVote)
    DefineUEnumValue(DifficultyIncrease)
    DefineUEnumValue(MissionActivation)
};

class EFortVoteArbitratorType
{
    DefineUnrealEnum(EFortVoteArbitratorType, FortniteGame)
    DefineUEnumValue(Invalid)
    DefineUEnumValue(Majority)
    DefineUEnumValue(Unanimous)

public:
    // 5.10-5.21 only (a vote that passes as soon as it ends: the widget's for a player who is alone); 5.30 dropped it
    // with FFortVoteConfig. No value (0xFF) where the build has none.
    static EFortVoteArbitratorType GetAutomaticPassIfAvailable() { return EFortVoteArbitratorType{ StaticEnumInfo().FindValue(L"AutomaticPass") }; }
};

class EFortVoteStatus
{
    DefineUnrealEnum(EFortVoteStatus, FortniteGame)
    DefineUEnumValue(Begin)
    DefineUEnumValue(Update)
    DefineUEnumValue(End)
    DefineUEnumValue(BeginLockout)
    DefineUEnumValue(EndLockout)
};

// One player's vote. VoteDecision is -1 until they vote, then 1 (yes) or 0 (no): UFortMissionActivationInfo's
// GetVoteCounts (5.10 0x2E29BD0) counts 1 and 0, HasPlayerVoted (0x2E2F4F0; 6.02 0x2FB8700) is "not -1".
// LastVoteDecision (RepSkip) is what OnRep_GameplayVotes last told the delegates. 5.30+: NumVotesCast, the votes the
// player has cast, up to the vote's MaxVotesAllowedPerPlayer.
struct FVoter
{
    DefineUnrealStruct(FVoter, FortniteGame)
    DefineUProperty(int32_t, VoteDecision)
    DefineUProperty(int32_t, LastVoteDecision)
    DefineUStructProperty(FUniqueNetIdRepl, NetId)
    DefineUProperty(int32_t, NumVotesCast)

public:
    static bool HasNumVotesCast() { return Member_NumVotesCast.IsAvailable(); }
};

// 5.30+ (CL-4305896 to 6.20 CL-4497486, 0x14 bytes): how a vote is decided, which the RPCs' ServerBeginGameplayVote
// takes and FVoteData keeps as VoteConfig. UFortMissionActivationInfo::RequestVote (6.02 0x2FCF9F0) sends two options,
// one vote each, and the widget's duration, lockout and arbitrator; the RPC's own default (exec 0x1538BC0) is two
// options, 45 s, no lockout, one vote, Majority. 5.0-5.21 pass the duration, lockout and arbitrator alone and keep them
// in FVoteData itself.
struct FFortVoteConfig
{
    DefineUnrealStruct(FFortVoteConfig, FortniteGame)
    DefineUProperty(int32_t, NumVoteOptions)
    DefineUProperty(float, VoteDuration)
    DefineUProperty(float, FailedVoteLockOutDuration)
    DefineUProperty(int32_t, MaxVotesAllowedPerPlayer)
    DefineUProperty(EFortVoteArbitratorType, VoteArbitratorType)

public:
    static bool IsAvailable()
    {
        return Member_NumVoteOptions.IsAvailable() && Member_VoteDuration.IsAvailable() && Member_FailedVoteLockOutDuration.IsAvailable()
            && Member_MaxVotesAllowedPerPlayer.IsAvailable() && Member_VoteArbitratorType.IsAvailable();
    }
};

struct FVoteData
{
    DefineUnrealStruct(FVoteData, FortniteGame)
    DefineUProperty(EFortVoteType, VoteType)
    DefineUStructProperty(FUniqueNetIdRepl, VoteInstigatorNetId)
    DefineUProperty(FDateTime, VoteStartTime)
    DefineUProperty(FDateTime, VoteEndTime)
    DefineUProperty(float, VoteDuration)
    DefineUProperty(float, FailedVoteLockOutDuration)
    DefineUProperty(EFortVoteArbitratorType, VoteArbitratorType)
    DefineUStructProperty(FFortVoteConfig, VoteConfig)
    DefineUStructArrayProperty(FVoter, Voters)
    DefineUProperty(int32_t, NumVotersWithMaxVotes)
    // The world's timer that ends the vote once VoteDuration has run out; once it has ended, Core's timer that drops it.
    DefineUProperty(FTimerHandle, EndVoteTimerHandle)
    DefineUProperty(EFortVoteStatus, VoteStatus)

public:
    static bool IsAvailable() { return Member_VoteStatus.IsAvailable() && Member_Voters.IsAvailable() && Member_EndVoteTimerHandle.IsAvailable(); }
    // 5.0-5.21 keep the vote's settings in FVoteData itself, as ServerBeginGameplayVote passes them.
    static bool HasVoteSettings() { return Member_VoteDuration.IsAvailable() && Member_FailedVoteLockOutDuration.IsAvailable() && Member_VoteArbitratorType.IsAvailable(); }
    // 5.30+ keep them as VoteConfig (FFortVoteConfig), with NumVotersWithMaxVotes, the voters who have cast all their
    // votes, and FVoter::NumVotesCast.
    static bool HasVoteConfig() { return Member_VoteConfig.IsAvailable() && Member_NumVotersWithMaxVotes.IsAvailable() && FFortVoteConfig::IsAvailable() && FVoter::HasNumVotesCast(); }

    // The vote's settings, wherever the build keeps them; 5.0-5.21's votes have two options and one vote per player.
    float GetVoteDuration() const { return HasVoteConfig() ? GetMember_VoteConfig().VoteDuration : GetMember_VoteDuration(); }
    float GetFailedVoteLockOutDuration() const { return HasVoteConfig() ? GetMember_VoteConfig().FailedVoteLockOutDuration : GetMember_FailedVoteLockOutDuration(); }
    EFortVoteArbitratorType GetVoteArbitratorType() const { return HasVoteConfig() ? GetMember_VoteConfig().VoteArbitratorType : GetMember_VoteArbitratorType(); }
    int32_t GetNumVoteOptions() const { return HasVoteConfig() ? GetMember_VoteConfig().NumVoteOptions : 2; }
    int32_t GetMaxVotesAllowedPerPlayer() const { return HasVoteConfig() ? GetMember_VoteConfig().MaxVotesAllowedPerPlayer : 1; }
    // LastVoteStatus, the TOptional<EFortVoteStatus> after VoteStatus (21.40's FVoteData has it as a member; 5.10's
    // OnRep_GameplayVotes, 0xE142E0, reads the value at +0x69 and its set flag at +0x6A, 6.02's, 0xE97120, at +0x79 and
    // +0x7A): no UPROPERTY, so never replicated, and OnRep_GameplayVotes is what sets it.
    void ResetLastVoteStatus()
    {
        uint8_t* const LastVoteStatus = reinterpret_cast<uint8_t*>(&GetMember_VoteStatus()) + 1;
        LastVoteStatus[0] = 0;
        LastVoteStatus[1] = 0;
    }
};

class AFortGameState : public AGameState
{
    DefineUnrealClass(AFortGameState, FortniteGame)

public:
    DefineUProperty(AFortMissionManager*, MissionManager)
    DefineUProperty(AFortWorldManager*, WorldManager)
    DefineUProperty(int32_t, WorldLevel)
    DefineUProperty(AFortVisibilityManager*, VisibilityManager)
    DefineUProperty(EFortGameplayState, GameplayState)
    DefineUFunction(Client_RefreshEventCalendar)
    // The playlist's AdditionalLevels that have streamed in, by package name: OnFinishedStreamingAdditionalPlaylistLevel
    // adds each as its level is shown (the server's own list, replicated).
    DefineUProperty(TArray<FName>, AdditionalPlaylistLevelsStreamed)
    static bool HasAdditionalPlaylistLevelsStreamed() { return Member_AdditionalPlaylistLevelsStreamed.IsAvailable(); }

    void SpawnMissionManager(const FFortMissionManagerRecord& MissionManagerRecord);
    // The game's own multicast (Net, NetReliable, NetMulticast): the server and every client fetch the event calendar
    // again at once, not at its cacheExpire (4.5's _Implementation, 0xC77C70, has the seasonal event manager's calendar
    // download it, 0xF20400). False where the build has no such function.
    bool Client_RefreshEventCalendar();

    // 21.40's MatchOptions (from 6.01; not reflected): the assignment's match options as the game mode's
    // OnGivenMatchAssignment left them, with each of the party's custom match options as a pair of its own
    // ("<scope>:<OptionKey>" and the value's text). Null where the game state has none (Natives::AFortGameState_MatchOptions).
    const TMap<FString, FString>* GetMatchOptions() const;

    static ptrdiff_t MissionManagerOffset();
};

class AFortGameStateZone : public AFortGameState
{
    DefineUnrealClass(AFortGameStateZone, FortniteGame)

public:
    DefineUProperty(float, GameDifficulty)
    // The BluGlo Difficulty Pylon's tier (2.5+; Mission_BluGloPylon's SetDifficultyIncreaseRewardTier).
    DefineUProperty(int32_t, DifficultyIncreaseRewardTier)
    DefineUProperty(TAssetSubclassOf<UFortMissionGenerator>, MissionGeneratorClass)
    DefineUProperty(FString, TheaterUniqueId)
    DefineUProperty(UFortZoneTheme*, ZoneTheme)
    DefineUProperty(UBuildingStructuralSupportSystem*, StructuralSupportSystem)
    DefineUProperty(int32_t, TotalPlayerStructures)
    DefineUProperty(int32_t, MaxPlayerStructures)
    // 5.0+: the gameplay votes (Mission Control's mission activation and difficulty increase votes).
    DefineUStructArrayProperty(FVoteData, GameplayVotesArray)
    DefineUFunction(OnRep_GameplayVotes)
    DefineUFunction(NotifyEndFailedGameplayVoteLockout)
    DefineUFunction(ServerSubmitGameplayVote)

    UFortMissionGenerator* GetMissionGenerator() const;
    // The difficulty increase tier the zone's BluGlo Difficulty Pylon reached, which EndPrimaryMission reports; 0 when it
    // wasn't used or the build has no pylon.
    int32_t GetDifficultyIncreaseRewardTier() const;
    // The zone's structure count and limit (-1: none). Building is refused once the count reaches the limit
    // (AFortPlayerController::CanPlaceBuildableClassInStructuralGrid, 1.8 0x816430); traps never ask.
    bool GetPlayerStructures(int32_t& OutTotal, int32_t& OutMax) const;
    // Sets the limit (MaxPlayerStructures, replicated: the HUD's "structures left" follows), as the game's own
    // AFortGameModeOutpost::ApplyStructureLimit does (1.8 0x57C440, a plain store). False without the property.
    bool SetMaxPlayerStructures(int32_t NewMax);
    void SetGameplayState(EFortGameplayState NewGameplayState);
    // The game's own (21.40's name; 5.41 0xE81110): on the authority, every ActiveGameplayModifiers entry's persistent
    // effects and ability sets go on the actor whose ability system this is, from the game state's global environment
    // ability actor, as each modifier's delivery requirements allow (a playlist's RedeployGlider_Modifier: player pawns).
    // The actors call it once their ability system is ready. False where the build has no such function (1.x).
    bool OnAbilitySystemInterfaceReadyForPersistentGameplayEffects(IAbilitySystemInterface* AbilitySystemInterface);

    // The gameplay votes' server side (5.0+, used from 5.10), which the build strips. 5.0-5.21: ServerBeginGameplayVote
    // keeps only its refusal of a second vote of a type (CL-4240749 0xE1DCF0), ServerSubmitGameplayVote and
    // ServerEndGameplayVote are the shared empty function. From 5.30 Begin takes an FFortVoteConfig, and 6.00-6.20 strip
    // all three (the 6.00, 6.01, 6.02, 6.10 and 6.20 IDBs; 6.02 CL-4461277: the execs 0x147A620, 0x147AB60 and
    // 0x147A7E0 call nullsub_3). AFortPlayerControllerZone's RPCs call them for the player's own net id.
    void ServerBeginGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId, float VoteDuration, float FailedVoteLockOutDuration, EFortVoteArbitratorType VoteArbitratorType);
    void ServerBeginGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId, const FFortVoteConfig& VoteConfig);
    void ServerSubmitGameplayVote(EFortVoteType VoteType, int32_t VoteDecision, const FUniqueNetIdRepl& PlayerNetId);
    void ServerEndGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId);
    // Whether this build has the votes and strips their server side.
    static bool AreGameplayVotesStripped();

private:
    FVoteData* FindGameplayVote(EFortVoteType VoteType);
    // The new vote both Begins make, every player in the zone a voter who hasn't voted; null when a vote of that type is
    // in progress or locked out.
    FVoteData* AddGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId);
    // Once a vote's settings are in: starts its EndVoteTimerHandle timer (the world's FTimerManager), which ends it when
    // VoteDuration runs out, and tells the delegates it began.
    void StartGameplayVote(EFortVoteType VoteType);
    // Once a vote has ended: the vote stays in the array while it is locked (IsGameplayVoteLocked, 6.02 0xE946B0, is
    // "in the array with VoteStatus End or later"), a moment for a passed one so every client sees it end, its
    // FailedVoteLockOutDuration for a failed one, and then goes (a failed one with NotifyEndFailedGameplayVoteLockout),
    // so the next vote of its type can begin.
    void ScheduleRemoveGameplayVote(EFortVoteType VoteType);
    // The vote's result: the decision its arbitrator picks from the voters', -1 for none. The build's own (5.10
    // 0xDFA840; 6.02 0xE7FA90, which takes the config's NumVoteOptions too; both called by OnRep_GameplayVotes) is: no
    // voters, -1; AutomaticPass (5.10-5.21), 1; Majority, the one decision whose voters times NumVoteOptions outnumber
    // the voters (with two options, more than half of them); Unanimous, the one decision every voter made.
    static int32_t GetVoteResult(EFortVoteArbitratorType VoteArbitratorType, int32_t NumVoteOptions, const TStructArray<FVoter>& Voters);
    // Runs OnRep_GameplayVotes on the server too, so the delegates bound on it (BindDelegateOnGameplayVoteUpdated: the
    // mission's BP_ReadyUp_Interact takes or returns the BluGlo and acts on the result) hear each change.
    void GameplayVotesChanged();
};
