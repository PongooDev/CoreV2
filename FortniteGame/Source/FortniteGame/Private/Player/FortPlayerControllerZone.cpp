#include "pch.h"
#include "Player/FortPlayerControllerZone.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortRuntimeOptions.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Log.h"
#include "Missions/FortMissionLibrary.h"
#include "Natives.h"
#include "Player/FortCheatManager.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState || GameState->GameplayState != EFortGameplayState::GetEndOfZone() || SetupTeleportOutCameraGameState.Get() == GameState)
    {
        return;
    }

    SetupTeleportOutCameraGameState = FWeakObjectPtr(GameState);
    UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"SetupTeleportOutCamera", FNAME_Find), nullptr);
#endif
}

void AFortPlayerControllerZone::HookPossessionVTables()
{
#if WITH_SERVER_CODE
    // A client that takes a pawn (ClientRestart) answers with ServerAcknowledgePossession, and the server keeps the pawn
    // as the controller's AcknowledgedPawn. The server moves a pawn only for a client that has acknowledged it:
    // UCharacterMovementComponent::ServerMove_Implementation (CL-4204761 0x22A6890) asks
    // APlayerController::NotifyServerReceivedClientData (0x2608D30, slot 0x191), which is false while AcknowledgedPawn
    // isn't the pawn, and then drops the move. 5.0 overrides _Implementation in this class (slot 0x104, the exec
    // 0x28C2DD0's last call) and strips it to the shared empty function, which Battle Royale's controller inherits;
    // 4.5's classes (CL-4166199) all kept APlayerController's (0x23A51C0). No 5.0 pawn was ever acknowledged, so the
    // server never moved one: it hung where it spawned, and after the bus it kept skydiving where it left the bus
    // (#345, #346, #347, #348). Its abilities were refused (Jump and Sprint need a pawn on the ground, interacting one
    // that isn't skydiving), auto pickup saw a pawn that never moved, and the glider couldn't open.
    UFunction* const Function = APlayerController::FindStaticFunction_ServerAcknowledgePossession();
    const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
    UClass* const SuperClass = StaticClass()->GetSuperClass();
    UObject* const SuperDefault = SuperClass ? SuperClass->GetDefaultObject(false) : nullptr;
    void* const Super = SuperDefault && Index >= 0 ? (*reinterpret_cast<void***>(SuperDefault))[Index] : nullptr;
    if (!Super || Finder::IsStubFunction(reinterpret_cast<uintptr_t>(Super)))
    {
        Log::Error(L"Finder: APlayerController::ServerAcknowledgePossession_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            ServerAcknowledgePossession_ImplementationSuper = reinterpret_cast<decltype(ServerAcknowledgePossession_ImplementationSuper)>(Super);
            Hooks::SwapVTable(L"AFortPlayerControllerZone::ServerAcknowledgePossession_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerAcknowledgePossession_ImplementationHK));
        }
    }
#endif
}

void AFortPlayerControllerZone::ServerAcknowledgePossession_ImplementationHK(AFortPlayerControllerZone* This, APawn* P)
{
    This->ServerAcknowledgePossession_Implementation(P);
}

void AFortPlayerControllerZone::ServerAcknowledgePossession_Implementation(APawn* P)
{
    // Super: APlayerController's, which sets AcknowledgedPawn.
    ServerAcknowledgePossession_ImplementationSuper(this, P);
#if WITH_SERVER_CODE
    // "cheat disguise" and "cheat possessplayer" (#154): the client has taken the pawn they gave it.
    UFortCheatManager::RestoreKeptView(this, P);
#endif
}

AFortGameStateZone* AFortPlayerControllerZone::GetVoteGameState(const FUniqueNetIdRepl*& OutPlayerNetId) const
{
    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    APlayerState* const Player = PlayerState;
    OutPlayerNetId = Player ? &Player->GetMember_UniqueId() : nullptr;
    return OutPlayerNetId ? GameState : nullptr;
}

void AFortPlayerControllerZone::ServerBeginGameplayVote_Implementation(EFortVoteType VoteType, float VoteDuration, float FailedVoteLockOutDuration, EFortVoteArbitratorType VoteArbitratorType)
{
    const FUniqueNetIdRepl* PlayerNetId = nullptr;
    if (AFortGameStateZone* GameState = GetVoteGameState(PlayerNetId))
    {
        GameState->ServerBeginGameplayVote(VoteType, *PlayerNetId, VoteDuration, FailedVoteLockOutDuration, VoteArbitratorType);
    }
}

void AFortPlayerControllerZone::ServerBeginGameplayVote_Implementation(EFortVoteType VoteType, const FFortVoteConfig& VoteConfig)
{
    const FUniqueNetIdRepl* PlayerNetId = nullptr;
    if (AFortGameStateZone* GameState = GetVoteGameState(PlayerNetId))
    {
        GameState->ServerBeginGameplayVote(VoteType, *PlayerNetId, VoteConfig);
    }
}

void AFortPlayerControllerZone::ServerSubmitGameplayVote_Implementation(EFortVoteType VoteType, int32_t VoteDecision)
{
    const FUniqueNetIdRepl* PlayerNetId = nullptr;
    if (AFortGameStateZone* GameState = GetVoteGameState(PlayerNetId))
    {
        GameState->ServerSubmitGameplayVote(VoteType, VoteDecision, *PlayerNetId);
    }
}

void AFortPlayerControllerZone::ServerEndGameplayVote_Implementation(EFortVoteType VoteType)
{
    const FUniqueNetIdRepl* PlayerNetId = nullptr;
    if (AFortGameStateZone* GameState = GetVoteGameState(PlayerNetId))
    {
        GameState->ServerEndGameplayVote(VoteType, *PlayerNetId);
    }
}

void AFortPlayerControllerZone::ServerBeginGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, float VoteDuration, float FailedVoteLockOutDuration, uint8_t VoteArbitratorType)
{
    This->ServerBeginGameplayVote_Implementation(EFortVoteType{ VoteType }, VoteDuration, FailedVoteLockOutDuration, EFortVoteArbitratorType{ VoteArbitratorType });
}

void AFortPlayerControllerZone::ServerBeginGameplayVoteConfig_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, const FFortVoteConfig* VoteConfig)
{
    if (VoteConfig)
    {
        This->ServerBeginGameplayVote_Implementation(EFortVoteType{ VoteType }, *VoteConfig);
    }
}

void AFortPlayerControllerZone::ServerSubmitGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, int32_t VoteDecision)
{
    This->ServerSubmitGameplayVote_Implementation(EFortVoteType{ VoteType }, VoteDecision);
}

void AFortPlayerControllerZone::ServerEndGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType)
{
    This->ServerEndGameplayVote_Implementation(EFortVoteType{ VoteType });
}

void AFortPlayerControllerZone::HookGameplayVoteVTables()
{
#if WITH_SERVER_CODE
    // 5.10 CL-4240749: the RPCs' execs (0x1452710, 0x1453040, 0x1452940) call the _Implementations in slots 0x37B, 0x379
    // and 0x377 (0x11AF870, 0x11B0430, 0x11AFB90), which find the game state and the player's UniqueId and call the game
    // state's stripped functions. 5.0 (CL-4204761) has the same code (OnRep_GameplayVotes 0xE03A50, the same FVoteData)
    // but its UFortRuntimeOptions::bEnableMissionActivationVote is off, so no mission uses it; builds before 5.0 have
    // no votes. 6.00-6.20 (and, by their SDKs, 5.30-5.41) have the same _Implementations, Begin's taking the
    // FFortVoteConfig its exec copies (6.02
    // CL-4461277: execs 0x1538BC0, 0x15393A0, 0x1538DA0, slots 0x397, 0x395, 0x393: 0x1241620, 0x12421D0, 0x1241930,
    // each calling nullsub_3 for the game state's function).
    if (!FindStaticFunction_ServerBeginGameplayVote() || !AFortGameStateZone::AreGameplayVotesStripped())
    {
        return;
    }
    // The settings Begin takes: 5.0-5.21's VoteDuration, FailedVoteLockOutDuration and VoteArbitratorType, or 5.30's
    // FFortVoteConfig (which the first would read as floats).
    const bool bVoteConfig = FVoteData::HasVoteConfig();
    if (!bVoteConfig && !FVoteData::HasVoteSettings())
    {
        Log::Warning(L"Mission Control: this build's vote settings are neither in FVoteData nor an FFortVoteConfig; gameplay votes stay as the build has them");
        return;
    }
    struct FVoteRestore
    {
        UFunction* Function;
        const wchar_t* Name;
        void* Implementation;
    };
    void* const BeginImplementation = bVoteConfig ? reinterpret_cast<void*>(&ServerBeginGameplayVoteConfig_ImplementationHK) : reinterpret_cast<void*>(&ServerBeginGameplayVote_ImplementationHK);
    const FVoteRestore Restores[] = {
        { StaticFunction_ServerBeginGameplayVote(), L"AFortPlayerControllerZone::ServerBeginGameplayVote_Implementation", BeginImplementation },
        { StaticFunction_ServerSubmitGameplayVote(), L"AFortPlayerControllerZone::ServerSubmitGameplayVote_Implementation", reinterpret_cast<void*>(&ServerSubmitGameplayVote_ImplementationHK) },
        { StaticFunction_ServerEndGameplayVote(), L"AFortPlayerControllerZone::ServerEndGameplayVote_Implementation", reinterpret_cast<void*>(&ServerEndGameplayVote_ImplementationHK) },
    };
    UObject* const ZoneDefault = StaticClass()->GetDefaultObject(false);
    void** const ZoneVTable = ZoneDefault ? *reinterpret_cast<void***>(ZoneDefault) : nullptr;
    for (const FVoteRestore& Restore : Restores)
    {
        const int32_t Index = Restore.Function ? Finder::ImplementationIndex(Restore.Function) : -1;
        void* const Original = ZoneVTable && Index >= 0 ? ZoneVTable[Index] : nullptr;
        if (!Original)
        {
            Log::Error(L"Finder: {} slot not found", Restore.Name);
            return;
        }
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Original)
            {
                Hooks::SwapVTable(Restore.Name, VTable, Index, Restore.Implementation);
            }
        }
    }
    const UFortRuntimeOptions* Options = UFortRuntimeOptions::Get();
    Log::Info(L"Mission Control: gameplay votes restored ({}; bEnableMissionActivationVote {})", bVoteConfig ? L"FFortVoteConfig" : L"FVoteData's settings",
        Options && UFortRuntimeOptions::HasMissionActivationVote() ? (Options->bEnableMissionActivationVote ? L"on" : L"off") : L"unknown");
#endif
}

void AFortPlayerControllerZone::HookVTables()
{
    HookPossessionVTables();
    HookGameplayVoteVTables();

    const int32_t Index = Finder::ImplementationIndex(StaticFunction_ServerTriggerSetupTeleportCameraEvent());
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerTriggerSetupTeleportCameraEvent_ImplementationHK));
        }
    }
}

void AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerZone* This)
{
    This->ServerTriggerSetupTeleportCameraEvent_Implementation();
}
