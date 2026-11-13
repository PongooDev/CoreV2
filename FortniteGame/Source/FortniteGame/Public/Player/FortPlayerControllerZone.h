#pragma once

#include "FortGameState.h"
#include "Player/FortPlayerController.h"
#include "UObject/WeakObjectPtr.h"

class AFortPlayerState;

// What a player's client is told of their death (ClientOnPawnDied), which the controller keeps as PlayerDeathReport.
// PrepareDeathReport (1.10 0x8DB450) fills the killer from the death's instigating controller: none for a fall or the
// storm. The 1.10 elimination screen (EliminatedOverlay) shows KillerPlayerState's Banner.
struct FFortPlayerDeathReport
{
    DefineUnrealStruct(FFortPlayerDeathReport, FortniteGame)
    DefineUProperty(AFortPlayerState*, KillerPlayerState)

public:
    static ptrdiff_t KillerPlayerStateOffset() { return Member_KillerPlayerState.GetOffsetChecked(); }
};

class AFortPlayerControllerZone : public AFortPlayerController
{
    DefineUnrealClass(AFortPlayerControllerZone, FortniteGame)
    DefineUFunction(ServerTriggerSetupTeleportCameraEvent)
    DefineUFunction(ServerBeginGameplayVote)
    DefineUFunction(ServerSubmitGameplayVote)
    DefineUFunction(ServerEndGameplayVote)

public:
    void ServerTriggerSetupTeleportCameraEvent_Implementation();
    // Mission Control's votes (5.0+, used from 5.10): the player's widget asks for them, and the game state, for the player's net id
    // (their player state's UniqueId), runs them. Begin takes the vote's settings alone on 5.0-5.21, an FFortVoteConfig
    // from 5.30.
    void ServerBeginGameplayVote_Implementation(EFortVoteType VoteType, float VoteDuration, float FailedVoteLockOutDuration, EFortVoteArbitratorType VoteArbitratorType);
    void ServerBeginGameplayVote_Implementation(EFortVoteType VoteType, const FFortVoteConfig& VoteConfig);
    void ServerSubmitGameplayVote_Implementation(EFortVoteType VoteType, int32_t VoteDecision);
    void ServerEndGameplayVote_Implementation(EFortVoteType VoteType);
    // The client's ServerAcknowledgePossession, sent once it has taken a new pawn (APlayerController::ClientRestart).
    void ServerAcknowledgePossession_Implementation(APawn* P);

    static void HookVTables();

private:
    static inline FWeakObjectPtr SetupTeleportOutCameraGameState;

    static void ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerZone* This);
    // Where the game state's vote functions are stripped (AFortGameStateZone::AreGameplayVotesStripped), the three
    // _Implementations, which only pass the player's net id to them, call Core's.
    static void HookGameplayVoteVTables();
    static void ServerBeginGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, float VoteDuration, float FailedVoteLockOutDuration, uint8_t VoteArbitratorType);
    // 5.30+: the exec passes a pointer to its copy of the RPC's FFortVoteConfig.
    static void ServerBeginGameplayVoteConfig_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, const FFortVoteConfig* VoteConfig);
    static void ServerSubmitGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, int32_t VoteDecision);
    static void ServerEndGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType);
    // The player's game state and net id, as the _Implementations find them (5.10 0x11AF870: the world's game mode's
    // AFortGameStateZone and the controller's player state's UniqueId).
    AFortGameStateZone* GetVoteGameState(const FUniqueNetIdRepl*& OutPlayerNetId) const;
    // Restores ServerAcknowledgePossession_Implementation where the build strips this class's override (5.0).
    static void HookPossessionVTables();
    static void ServerAcknowledgePossession_ImplementationHK(AFortPlayerControllerZone* This, APawn* P);
    // The super class's (AFortPlayerController's, which is APlayerController's) ServerAcknowledgePossession_Implementation.
    static inline void (*ServerAcknowledgePossession_ImplementationSuper)(AFortPlayerControllerZone* This, APawn* P) = nullptr;

public:
    // Core runs the controllers' ServerAcknowledgePossession_Implementation (5.0+), so it sees a client take its pawn.
    static bool HasServerAcknowledgePossession() { return ServerAcknowledgePossession_ImplementationSuper != nullptr; }
};
