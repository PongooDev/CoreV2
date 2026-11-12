#pragma once

#include "Containers/Map.h"
#include "Containers/UnrealString.h"
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
    DefineUEnumValue(AutomaticPass)
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
// GetVoteCounts (5.10 0x2E29BD0) counts 1 and 0, HasPlayerVoted (0x2E2F4F0) is "not -1". LastVoteDecision (RepSkip) is
// what OnRep_GameplayVotes last told the delegates.
struct FVoter
{
    DefineUnrealStruct(FVoter, FortniteGame)
    DefineUProperty(int32_t, VoteDecision)
    DefineUProperty(int32_t, LastVoteDecision)
    DefineUStructProperty(FUniqueNetIdRepl, NetId)
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
    DefineUStructArrayProperty(FVoter, Voters)
    DefineUProperty(EFortVoteStatus, VoteStatus)

public:
    static bool IsAvailable() { return Member_VoteStatus.IsAvailable() && Member_Voters.IsAvailable(); }
    // 5.0-5.21 keep the vote's settings in FVoteData itself, as ServerBeginGameplayVote passes them. 5.30 (CL-4305896)
    // passes an FFortVoteConfig instead (NumVoteOptions, VoteDuration, FailedVoteLockOutDuration,
    // MaxVotesAllowedPerPlayer, VoteArbitratorType), which FVoteData keeps as VoteConfig, with NumVotersWithMaxVotes
    // and FVoter::NumVotesCast for votes of more than one option.
    static bool HasVoteSettings() { return Member_VoteDuration.IsAvailable() && Member_FailedVoteLockOutDuration.IsAvailable() && Member_VoteArbitratorType.IsAvailable(); }
    // LastVoteStatus, the TOptional<EFortVoteStatus> after VoteStatus (21.40's FVoteData has it as a member; 5.10's
    // OnRep_GameplayVotes, 0xE142E0, reads the value at +0x69 and its set flag at +0x6A): no UPROPERTY, so never
    // replicated, and OnRep_GameplayVotes is what sets it.
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

    // The gameplay votes' server side (5.0+, used from 5.10), which the build strips: ServerBeginGameplayVote keeps only its refusal of
    // a second vote of a type (CL-4240749 0xE1DCF0), ServerSubmitGameplayVote and ServerEndGameplayVote are the shared
    // empty function. AFortPlayerControllerZone's RPCs call them for the player's own net id.
    void ServerBeginGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId, float VoteDuration, float FailedVoteLockOutDuration, EFortVoteArbitratorType VoteArbitratorType);
    void ServerSubmitGameplayVote(EFortVoteType VoteType, int32_t VoteDecision, const FUniqueNetIdRepl& PlayerNetId);
    void ServerEndGameplayVote(EFortVoteType VoteType, const FUniqueNetIdRepl& PlayerNetId);
    // Whether this build has the votes and strips their server side.
    static bool AreGameplayVotesStripped();
    // Server, before each frame's replication (UNetDriver::AddPreReplicationCallback): ends the world's votes whose
    // VoteDuration has run out (FVoteData::EndVoteTimerHandle's timer), and drops ended ones (a failed one once its
    // FailedVoteLockOutDuration has run out, with NotifyEndFailedGameplayVoteLockout).
    static void TickGameplayVotes();

private:
    FVoteData* FindGameplayVote(EFortVoteType VoteType);
    // The vote's result: the decision its arbitrator picks from the voters', -1 for none. The build's own (5.10
    // 0xDFA840, which OnRep_GameplayVotes calls) is: no voters, -1; AutomaticPass, 1; Majority, the decision more than
    // half the voters made; Unanimous, the one decision every voter made.
    static int32_t GetVoteResult(EFortVoteArbitratorType VoteArbitratorType, const TStructArray<FVoter>& Voters);
    // Runs OnRep_GameplayVotes on the server too, so the delegates bound on it (BindDelegateOnGameplayVoteUpdated: the
    // mission's BP_ReadyUp_Interact takes or returns the BluGlo and acts on the result) hear each change.
    void GameplayVotesChanged();
};
