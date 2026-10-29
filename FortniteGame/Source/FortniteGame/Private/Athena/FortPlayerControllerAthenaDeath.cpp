#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameThread.h"
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPawn.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/WeakObjectPtr.h"

#include <vector>

// 1.9.1 (CL-3775276) strips AFortPlayerControllerAthena's pawn-death handler, which the pawn calls when it dies (1.9
// 0x42DB20, slot 0x305): its slot (0x308, 0x42DA80) is a jump to AFortPlayerControllerZone's handler. A player who
// dies stays among the alive players (AFortGameModeAthena::RemoveFromAlivePlayers is intact but only Logout calls it),
// PlayersLeft and TeamsLeft never fall, nobody wins, and the dead player gets no death report, time alive or place
// (Core's match results read Place). This restores 1.9's body where the slot is stripped, calling 1.9.1's own pieces
// (Finder::ResolveAthenaPawnDiedLive), which its wins (0x438B50, 0x438DD0) and its DBNO handling (0x438640) still call.
// It leaves out what 1.9.1 took out of those too: the eliminator's list of kill distances (1.9's player state +0xDE0,
// gone from 1.9.1's layout and its DBNO handling) and the "Combat.PlayerDeath" analytics event (1.9 0x38D6B0; 1.9.1's
// wins call a stub for theirs).

DEFINE_LOG_CATEGORY_STATIC(LogFortDeath, Log, All);

namespace
{
    // 1.9's spectate timer: SpectateOnDeath five seconds after the death, then every two (FTimerManager::SetTimer with
    // the controller's native handle +0x32B0, rate 2, looping, first delay 5).
    constexpr float SpectateFirstDelay = 5.0f;
    constexpr float SpectateRate = 2.0f;

    struct FPendingSpectate
    {
        TWeakObjectPtr<AFortPlayerControllerAthena> Controller;
        float StartTime = 0.0f;
    };

    std::vector<FPendingSpectate> PendingSpectates;
    bool bWatchingSpectates = false;

    void StartSpectating(AFortPlayerControllerAthena* Controller)
    {
        Reflection::FParams Params(AFortPlayerControllerAthena::StaticFunction_SpectateOnDeath());
        Params.Call(Controller);
        UKismetSystemLibrary::K2_SetTimer(Controller, L"SpectateOnDeath", SpectateRate, true);
    }

    void WatchSpectates()
    {
        std::erase_if(PendingSpectates, [](const FPendingSpectate& Pending)
        {
            AFortPlayerControllerAthena* Controller = Pending.Controller.Get();
            UWorld* World = Controller ? Controller->GetWorld() : nullptr;
            if (!World)
            {
                return true;
            }
            if (World->GetTimeSeconds() < Pending.StartTime)
            {
                return false;
            }
            StartSpectating(Controller);
            return true;
        });
        bWatchingSpectates = !PendingSpectates.empty() && GameThread::Enqueue(&WatchSpectates);
    }

    void SpectateOnDeathSoon(AFortPlayerControllerAthena* Controller)
    {
        UWorld* World = Controller->GetWorld();
        if (!World)
        {
            return;
        }
        UKismetSystemLibrary::K2_ClearTimer(Controller, L"SpectateOnDeath");
        std::erase_if(PendingSpectates, [Controller](const FPendingSpectate& Pending) { return Pending.Controller.Get() == Controller; });
        PendingSpectates.push_back({ TWeakObjectPtr<AFortPlayerControllerAthena>(Controller), World->GetTimeSeconds() + SpectateFirstDelay });
        if (!bWatchingSpectates)
        {
            bWatchingSpectates = GameThread::Enqueue(&WatchSpectates);
        }
    }

    AFortPlayerStateAthena* AthenaStateOf(AController* Controller)
    {
        return Controller ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState))) : nullptr;
    }
}

void AFortPlayerControllerAthena::HookVTables()
{
#if WITH_SERVER_CODE
    const int32_t Index = Finder::StrippedAthenaPawnDiedIndex();
    if (Index < 0)
    {
        return;
    }
    if (!Finder::ResolveAthenaPawnDiedLive(AFortPlayerStateAthena::PlaceOffset(), AFortPlayerStateAthena::SecondsAliveOffset(),
        AFortPlayerStateAthena::DeathInfoOffset() + FDeathInfo::DeathCauseOffset(), AFortPlayerStateAthena::DeathInfoOffset() + FDeathInfo::DBNOOffset(), TeamMembersOffset()))
    {
        Log::Error(L"Battle Royale: this build strips the pawn-death handler and its pieces were not found, so nobody who dies is out of the match");
        return;
    }

    // Only the slots that hold the stripped handler: the class's own vtable and its Blueprint subclasses', which share it.
    UObject* AthenaDefault = StaticClass()->GetDefaultObject(false);
    void* const Stripped = AthenaDefault ? (*reinterpret_cast<void***>(AthenaDefault))[Index] : nullptr;
    for (TObjectIterator<UClass> It; It && Stripped; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Stripped)
        {
            Hooks::SwapVTable(L"AFortPlayerControllerAthena::OnPawnDied", VTable, Index, reinterpret_cast<void*>(&OnPawnDiedHK));
        }
    }
#endif
}

void AFortPlayerControllerAthena::OnPawnDiedHK(AFortPlayerControllerAthena* This, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer)
{
    This->OnPawnDied(Damage, DamageTags, Momentum, Instigator, DamageCauser, Downer);
}

bool AFortPlayerControllerAthena::IsTeamOut()
{
    // 1.9 0x42A310: not before the aircraft has gone, then once none of TeamMembers has a pawn that is still in play (a
    // dead pawn is torn off before its controller hears of the death, 1.9.1 0x426B90).
    UWorld* World = GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (GameState && GameState->GamePhase <= EAthenaGamePhase::GetAircraft())
    {
        return false;
    }
    const TArray<AFortPlayerStateAthena*>& Members = TeamMembers;
    for (int32_t MemberIndex = 0; MemberIndex < Members.Num(); ++MemberIndex)
    {
        AFortPlayerStateAthena* Member = Members[MemberIndex];
        AFortPlayerPawn* MemberPawn = Member ? Member->GetCurrentPawn() : nullptr;
        if (MemberPawn && !MemberPawn->bTearOff)
        {
            return false;
        }
    }
    return true;
}

void AFortPlayerControllerAthena::OnPawnDied(float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer)
{
    // 1.9 0x42DB20. Zone's handler first: the death report and ClientOnPawnDied, the inventory drop and where the
    // controller stands (Core's, PawnDiedInventoryDropCall).
    Natives::AFortPlayerControllerZone_OnPawnDied(this, Damage, DamageTags, Momentum, Instigator, DamageCauser, Downer);

#if WITH_SERVER_CODE
    AFortPawn* DeadPawn = MyFortPawn;
    AFortPlayerStateAthena* AthenaState = AthenaStateOf(this);
    if (AthenaState)
    {
        // The death report the death screen shows (OnRep_DeathInfo): how the player died, and who downed them, else who
        // eliminated them, else themselves. The eliminator's client gets its kill feed line.
        FDeathInfo& DeathInfo = AthenaState->DeathInfo;
        if (DeadPawn && Natives::AFortPlayerPawn_GetDeathCause)
        {
            DeathInfo.DeathCause = Natives::AFortPlayerPawn_GetDeathCause(DeadPawn, DamageTags, DeadPawn->bIsDBNO);
        }
        if (Instigator)
        {
            DeathInfo.FinisherOrDowner = AthenaStateOf(Downer ? Downer : Instigator);
            DeathInfo.bDBNO = false;
            if (AFortPlayerStateAthena* KillerState = AthenaStateOf(Instigator))
            {
                KillerState->ClientReportKill(AthenaState->GetPlayerName());
            }
        }
        else
        {
            DeathInfo.FinisherOrDowner = AthenaState;
        }
    }

    UWorld* World = GetWorld();
    if (Role != ROLE_Authority || !World)
    {
        return;
    }
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this);
    APawn* KillerPawn = Instigator ? Instigator->GetPawn() : nullptr;
    if (GameMode && GameMode->bAllowSpectateAfterDeath)
    {
        if (Instigator)
        {
            PlayerToSpectateOnDeath = KillerPawn;
        }
        SpectateOnDeathSoon(this);
    }
    if (AFortPlayerPawnAthena* AthenaPawn = Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(DeadPawn)); AthenaPawn && Instigator)
    {
        AthenaPawn->KillerForSpectatorRotation = KillerPawn;
    }

    // Out of the match, once the aircraft is under way, if the player was in it.
    AFortGameStateAthena* GameState = Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState()));
    if (!GameState || !GameMode || GameState->GamePhase <= EAthenaGamePhase::GetWarmup() || !bMarkedAlive || !AthenaState)
    {
        return;
    }
    AthenaState->SecondsAlive = Natives::AFortPlayerStateAthena_GetSecondsAlive(AthenaState);
    if (Natives::AFortPlayerControllerAthena_CountTeammatesOut)
    {
        // What the player outlived (a native count of the player state, copied with it by 1.9 0x450360). 1.9 sets the
        // starting players less those left (this player among them). 1.9.1's handler is gone, but its wins leave out the
        // winner's teammates who are out, and its team win only sets the count of a player who has none (-1), one who
        // died before: so the same count less the teammates who are out.
        const int32_t StartingPlayers = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(GameState) + Natives::AFortGameStateAthena_StartingPlayers);
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(AthenaState) + Natives::AFortPlayerStateAthena_PlayersOutlived) = StartingPlayers - Natives::AFortPlayerControllerAthena_CountTeammatesOut(this) - GameState->PlayersLeft;
    }
    UE_LOG(LogFortDeath, Log, TEXT("FortGameModeAthena: Player [%s] killed after %d seconds.  Removing from alive players list."), *AthenaState->GetPlayerName(), static_cast<int32_t>(AthenaState->SecondsAlive));

    // The team's place is the teams left when its last player is out (before this one is taken off the count).
    if (GameMode->bSquadPlay && Natives::AFortPlayerControllerAthena_UpdateTeamMembersIndex >= 0)
    {
        CallVirtual<void>(this, Natives::AFortPlayerControllerAthena_UpdateTeamMembersIndex);
    }
    if (IsTeamOut())
    {
        Natives::AFortPlayerControllerAthena_SetTeamPlace(this, GameState->TeamsLeft);
    }
    Natives::AFortGameModeAthena_RemoveFromAlivePlayers(GameMode, this, Instigator ? static_cast<APlayerState*>(Instigator->PlayerState) : nullptr);
    UE_LOG(LogFortDeath, Log, TEXT("OnPawnDied: %s is out (place %d, %d players and %d teams left)"), *GetName(), static_cast<int32_t>(AthenaState->Place), static_cast<int32_t>(GameState->PlayersLeft), static_cast<int32_t>(GameState->TeamsLeft));

    Reflection::FParams Params(StaticFunction_ClientFinishedInteractionInZone());
    Params.Call(this);
#endif
}
