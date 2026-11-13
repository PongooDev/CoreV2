#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/AI/CoreAthenaBotManager.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPawn.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/FortWeapon.h"
#include "UObject/WeakObjectPtr.h"

#include <utility>
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
    // A team game: the game mode's bSquadPlay up to 4.2. 4.4 drops it and reads the playlist instead: its team win
    // (4.4.2 0xA74DB0, RemoveFromAlivePlayers' last call) checks "cmp [CurrentPlaylistData + MaxTeamSize], 1; jle".
    bool IsSquadPlay(AFortGameModeAthena* GameMode, AFortGameStateAthena* GameState)
    {
        if (AFortGameModeAthena::HasSquadPlay())
        {
            return GameMode->bSquadPlay;
        }
        UFortPlaylist* Playlist = AFortGameStateAthena::HasCurrentPlaylist() && UFortPlaylist::HasMaxTeamSize() ? static_cast<UFortPlaylist*>(GameState->GetCurrentPlaylist()) : nullptr;
        return Playlist && Playlist->MaxTeamSize > 1;
    }

    // 1.9's spectate timer: SpectateOnDeath five seconds after the death, then every two (FTimerManager::SetTimer with
    // the controller's native handle +0x32B0, rate 2, looping, first delay 5).
    constexpr float SpectateFirstDelay = 5.0f;
    constexpr float SpectateRate = 2.0f;

    // The spectate timer's handle for each controller that died: 1.9 keeps it on the controller (+0x32B0, a native
    // FTimerHandle no build reflects), so a later death's timer replaces the last one's.
    std::vector<std::pair<TWeakObjectPtr<AFortPlayerControllerAthena>, FTimerHandle>> SpectateOnDeathTimerHandles;

    FTimerHandle* FindSpectateOnDeathTimerHandle(AFortPlayerControllerAthena* Controller)
    {
        std::erase_if(SpectateOnDeathTimerHandles, [](const auto& Entry) { return !Entry.first.Get(true); });
        const TWeakObjectPtr<AFortPlayerControllerAthena> WeakController(Controller);
        for (auto& [Owner, Handle] : SpectateOnDeathTimerHandles)
        {
            if (Owner == WeakController)
            {
                return &Handle;
            }
        }
        return nullptr;
    }

    FTimerHandle& SpectateOnDeathTimerHandleOf(AFortPlayerControllerAthena* Controller)
    {
        FTimerHandle* Found = FindSpectateOnDeathTimerHandle(Controller);
        return Found ? *Found : SpectateOnDeathTimerHandles.emplace_back(TWeakObjectPtr<AFortPlayerControllerAthena>(Controller), FTimerHandle()).second;
    }

    AFortPlayerStateAthena* AthenaStateOf(AController* Controller)
    {
        return Controller ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState))) : nullptr;
    }
}

bool FDeathInfo::FinisherOrDownerIsAnyActor()
{
    static const bool bAnyActor = []
    {
        UScriptStruct* const Struct = FDeathInfo::StaticStruct();
        const UProperty* const Property = Struct ? Struct->FindPropertyByName(FName(L"FinisherOrDowner", FNAME_Find)) : nullptr;
        return Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UObjectProperty)
            && static_cast<const UObjectProperty*>(Property)->PropertyClass == AActor::StaticClass();
    }();
    return bAnyActor;
}

void AFortPlayerControllerAthena::SpectateOnDeath()
{
    Reflection::FParams Params(StaticFunction_SpectateOnDeath());
    Params.Call(this);
}

void AFortPlayerControllerAthena::StopSpectatingOnDeath()
{
#if WITH_SERVER_CODE
    // The death's timer loops (rate 2) until the controller goes: once the player is back, each run would only find a
    // view target that is alive (4.5 0xAAC400 returns there unless that pawn drives a remote-controlled one).
    UWorld* World = GetWorld();
    FTimerHandle* Handle = World ? FindSpectateOnDeathTimerHandle(this) : nullptr;
    if (Handle && Handle->IsValid())
    {
        World->GetTimerManager().ClearTimer(*Handle);
    }
    // SpectateOnDeath's BeginSpectating (4.5 0xAC1630, with bEnableSpectatorUpdates) put the player among the watched
    // player's spectators, the HUD's "N spectating"; nothing in the respawn takes them out (the game's only other
    // EndSpectating caller, 4.5 0xA7CC40, is the game mode's logout).
    AFortPlayerStateAthena* AthenaState = AthenaStateOf(this);
    if (AthenaState && AFortPlayerStateAthena::HasSpectators() && static_cast<AFortPlayerStateAthena*>(AthenaState->SpectatingTarget))
    {
        UE_LOG(LogFortDeath, Log, TEXT("StopSpectatingOnDeath: %s is back and stops spectating %s"), *GetName(), *static_cast<AFortPlayerStateAthena*>(AthenaState->SpectatingTarget)->GetPlayerName());
        AthenaState->EndSpectating();
    }
#endif
}

// Every player still in the match outlived the one who is out: an EFortQuestObjectiveStatEvent::AthenaOutlive event
// for each, unflushed, which the "Outlive opponents" challenges count (3.0's outlive_athena, "Outlive 1000 opponents";
// 1.8's dailies). 1.8 sends one for every other player's death (0x599DA0); 3.0 has no sender of the event left: its
// native stat event senders (the 11 calls to the stripped typed SendStatEvent) send Interact, Visit, Land, Damage,
// kills, team events (Build, Destroy) and WaveComplete only (Core-Backend #214). Sent here, with the player taken off
// the alive players, which only Core's handler does on the builds that strip the native one. A player bot's elimination
// sends them too (FCoreAthenaBotManager::RemoveFromAliveBots, CoreV2 #233).
void AFortGameModeAthena::SendOutliveStatEvents(AFortPawn* DeadPawn)
{
    const EFortQuestObjectiveStatEvent AthenaOutlive{ EFortQuestObjectiveStatEvent::StaticEnumInfo().FindValue(L"AthenaOutlive") };
    if (static_cast<uint8_t>(AthenaOutlive) == Reflection::InvalidEnumValue)
    {
        return;
    }
    const FGameplayTagContainer NoTargetTags;
    const TArray<AFortPlayerControllerAthena*>& Alive = AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Survivor = Alive[Index];
        UFortRegisteredPlayerInfo* PlayerInfo = Survivor ? static_cast<UFortRegisteredPlayerInfo*>(Survivor->MyPlayerInfo) : nullptr;
        UFortQuestManager* QuestManager = PlayerInfo ? PlayerInfo->GetAthenaQuestManager() : nullptr;
        if (!QuestManager)
        {
            continue;
        }
        FGameplayTagContainer SourceTags;
        FGameplayTagContainer ContextTags;
        QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
        QuestManager->SendStatEvent(AthenaOutlive, DeadPawn, NoTargetTags, SourceTags, ContextTags, 1, false);
    }
}

void AFortPlayerControllerAthena::HookVTables()
{
#if WITH_SERVER_CODE
    HookQuickChatVTables();

    // Only the slots that hold the stripped function: the class's own vtable and its Blueprint subclasses', which share it.
    const auto SwapStripped = [](const wchar_t* Name, int32_t Index, void* Replacement)
    {
        UObject* AthenaDefault = StaticClass()->GetDefaultObject(false);
        void* const Stripped = AthenaDefault ? (*reinterpret_cast<void***>(AthenaDefault))[Index] : nullptr;
        for (TObjectIterator<UClass> It; It && Stripped; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Stripped)
            {
                Hooks::SwapVTable(Name, VTable, Index, Replacement);
            }
        }
    };

    if (Member_PlayerDeathReport.IsAvailable() && FFortPlayerDeathReport::StaticStruct())
    {
        const int32_t ReportIndex = Finder::StrippedAthenaPrepareDeathReportIndex(PlayerDeathReportOffset() + FFortPlayerDeathReport::KillerPlayerStateOffset());
        if (ReportIndex >= 0)
        {
            SwapStripped(L"AFortPlayerControllerAthena::PrepareDeathReport", ReportIndex, reinterpret_cast<void*>(&PrepareDeathReportHK));
        }
    }

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
    SwapStripped(L"AFortPlayerControllerAthena::OnPawnDied", Index, reinterpret_cast<void*>(&OnPawnDiedHK));
    // The player state's own store of a death, which the restored handler hands its FDeathInfo to (OnPawnDied). From
    // 6.01 only, the builds whose game state reflects bAllowRespawning (as AFortGameModeAthena's IsRespawningAllowed
    // finder has it): every reader of a stored death was read in 6.01's and 6.02's images, no older build's, and
    // bInitialized (from 4.4) changes what each of them does. Older builds keep the report written field by field.
    if (AFortGameStateAthena::HasAllowRespawning() && FDeathInfo::HasInitialized() && Natives::AFortPlayerStateAthena_ToDeathCause)
    {
        const ptrdiff_t DeathInfoOffset = AFortPlayerStateAthena::DeathInfoOffset();
        const ptrdiff_t DeathCircumstanceOffset = FDeathInfo::DeathCircumstanceOffset();
        Finder::ResolveSetDeathInfoLive(DeathInfoOffset, DeathInfoOffset + FDeathInfo::DeathCauseOffset(), DeathInfoOffset + FDeathInfo::InitializedOffset(),
            DeathCircumstanceOffset >= 0 ? DeathInfoOffset + DeathCircumstanceOffset : -1);
    }
#endif
}

void AFortPlayerControllerAthena::PrepareDeathReportHK(AFortPlayerControllerAthena* This, float LethalDamage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* KillerController, AActor* DamageCauser, bool bDroppedBackpack)
{
    This->PrepareDeathReport(LethalDamage, DamageTags, Momentum, KillerController, DamageCauser, bDroppedBackpack);
}

void AFortPlayerControllerAthena::PrepareDeathReport(float LethalDamage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* KillerController, AActor* DamageCauser, bool bDroppedBackpack)
{
    // 1.10 0x449F80: AFortPlayerControllerZone's report, then the Athena game mode's changes to it (stripped).
    Natives::AFortPlayerControllerZone_PrepareDeathReport(this, LethalDamage, DamageTags, Momentum, KillerController, DamageCauser, bDroppedBackpack);
#if WITH_SERVER_CODE
    if (AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this))
    {
        GameMode->ModifyDeathReport(this, PlayerDeathReport);
    }
#endif
}

void AFortPlayerControllerAthena::OnPawnDiedHK(AFortPlayerControllerAthena* This, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer)
{
    This->OnPawnDied(Damage, DamageTags, Momentum, Instigator, DamageCauser, Downer);
}

float AFortPlayerPawnAthena::GetDeathInfoDistance(AFortPawn* Victim, APawn* FinisherPawn, const FGameplayTagContainer& DamageTags)
{
    // 5.10 0xC11510: 0, the eliminator's pawn's distance when someone else did it, then the pawn's LastFallDistance when
    // the damage tags hold FortGlobals' falling damage tag (the one ToDeathCause, 0xC5F0D0, turns into FallDamage). The
    // client's kill feed always shows the distance for a fall ("didn't stick the landing (N m)", OnRep_DeathInfo 0xC492D0).
    float Distance = FinisherPawn && Victim ? FinisherPawn->GetDistanceTo(Victim) : 0.0f;
    if (HasLastFallDistance())
    {
        static const FName FallingTag(L"Gameplay.Damage.Environment.Falling", FNAME_Find);
        AFortPlayerPawnAthena* AthenaVictim = Victim ? Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Victim)) : nullptr;
        if (AthenaVictim && !FallingTag.IsNone() && DamageTags.HasTag(FGameplayTag{ FallingTag }))
        {
            Distance = AthenaVictim->LastFallDistance;
        }
    }
    return Distance;
}

bool AFortPlayerControllerAthena::IsTeamOut()
{
    // 1.9 0x42A310: not before the aircraft has gone, then once none of TeamMembers has a pawn that is still in play (a
    // dead pawn is torn off before its controller hears of the death, 1.9.1 0x426B90). 1.11 moved only the HUD's list to
    // SquadMembers: TeamMembers is still the team the place and the wins are for (OnPawnDied fills it), and the team is
    // out as the game counts it, when no one on the team stands (2.1's count of the team's standing players, 0x43BC30,
    // reads TeamMembers; its DBNO handling, 0x48F2E0, looks for a teammate standing on the team, not the squad).
    UWorld* World = GetWorld();
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (GameState && GameState->GamePhase <= EAthenaGamePhase::GetAircraft())
    {
        return false;
    }
    // Battle Royale's player bots in the team's empty places ("Fill players' teams with bots", CoreV2 #233) keep it in:
    // its place is the bots' manager's to give once the last of them is out (FCoreAthenaBotManager::RemoveFromAliveBots).
    const FCoreAthenaBotManager* BotManager = FCoreAthenaBotManager::Get();
    const AFortPlayerStateAthena* AthenaState = AthenaStateOf(this);
    if (BotManager && AthenaState && BotManager->HasPlayersTeamBotsLeft(static_cast<uint8_t>(AthenaState->TeamIndex)))
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
    // 3.5 keeps this much of its own body (3.5.1 0xA27A00): the player's map marker goes on the server.
    if (Natives::AFortPlayerControllerAthena_RemoveMapCursor && Role == ROLE_Authority)
    {
        Natives::AFortPlayerControllerAthena_RemoveMapCursor(this);
    }

#if WITH_SERVER_CODE
    AFortPawn* DeadPawn = MyFortPawn;
    AFortPlayerStateAthena* AthenaState = AthenaStateOf(this);
    uint8_t DeathCause = EDeathCause::GetUnspecified();
    if (AthenaState)
    {
        // The death report the death screen shows (OnRep_DeathInfo): how the player died, and who downed them, else who
        // eliminated them, else themselves. The eliminator's client gets its kill feed line.
        // From 6.01 it is stored as the game stores one (HasSetDeathInfo): built apart, as the pawn's DBNO handling
        // builds a knock's on its stack (6.02 0xC5BE12), and handed whole to the player state's own store (0xC4A090),
        // which sets bInitialized. The stripped handler was that store's other caller: its kill feed line prints bDBNO
        // as true or false, and the knock, the one caller left, only ever stores true. Without bInitialized the death is
        // none to the code that brings a player back (the playlists that do have no DBNO, so nothing else set it):
        // IsRespawningAllowed's storm rule (6.02 0xBD08A0), which the pawn's death handler asks right after this one
        // (0xC486D0), FinishRestartPlayer's move of the new pawn to the respawn place (0xBC7430: the place the death
        // handler worked out and sent the client to load, a mutator's in Disco Domination, 0xC77900), the playspace
        // manager's (0x1262560, 0x1280EC0), and to IsPlayerDead (0xC4AC80: the squad bar, spectating's next teammate).
        // The fields start from the player state's, so one this handler leaves alone is stored as it was.
        const bool bSetDeathInfo = AFortPlayerStateAthena::HasSetDeathInfo();
        FDeathInfo& StoredDeathInfo = AthenaState->DeathInfo;
        std::vector<uint8_t> NewDeathInfo;
        if (bSetDeathInfo)
        {
            const uint8_t* const Stored = reinterpret_cast<const uint8_t*>(&StoredDeathInfo);
            NewDeathInfo.assign(Stored, Stored + FDeathInfo::StaticStruct()->GetStructureSize());
        }
        FDeathInfo& DeathInfo = bSetDeathInfo ? *reinterpret_cast<FDeathInfo*>(NewDeathInfo.data()) : StoredDeathInfo;
        if (DeadPawn && AFortPlayerStateAthena::HasToDeathCause())
        {
            DeathCause = AFortPlayerStateAthena::ToDeathCause(DeadPawn, DamageTags, DeadPawn->bIsDBNO);
            DeathInfo.DeathCause = DeathCause;
        }
        // How far away the eliminator was, or how far the player fell, as the DBNO handling has it (4.1 0xAAE559,
        // 0xAAE619; 5.10 0xC11510).
        if (FDeathInfo::HasDistance())
        {
            AController* Finisher = Downer ? Downer : Instigator;
            APawn* FinisherPawn = Finisher && Finisher != this ? Finisher->GetPawn() : nullptr;
            DeathInfo.Distance = AFortPlayerPawnAthena::GetDeathInfoDistance(DeadPawn, FinisherPawn, DamageTags);
        }
        if (Instigator)
        {
            AController* const Finisher = Downer ? Downer : Instigator;
            DeathInfo.FinisherOrDowner = AthenaStateOf(Finisher);
            // From 6.20 (an AActor there) the game stores the finisher's player state, else its pawn, else the player's
            // own (the DBNO handling's knock, 0xCE8D20), so an AI without a player state, a Cube monster, is named too.
            if (FDeathInfo::FinisherOrDownerIsAnyActor())
            {
                APlayerState* const FinisherState = Finisher->PlayerState;
                APawn* const FinisherPawn = Finisher->GetPawn();
                DeathInfo.FinisherOrDowner = FinisherState ? reinterpret_cast<AActor*>(FinisherState) : FinisherPawn ? reinterpret_cast<AActor*>(FinisherPawn) : AthenaState;
            }
            DeathInfo.bDBNO = false;
            if (AFortPlayerStateAthena* KillerState = AthenaStateOf(Instigator))
            {
                KillerState->ClientReportKill(AthenaState);
            }
        }
        else
        {
            DeathInfo.FinisherOrDowner = AthenaState;
        }
        if (FDeathInfo::HasDeathLocation() && DeadPawn)
        {
            DeathInfo.DeathLocation = DeadPawn->GetTransform().GetLocation();
        }
        if (bSetDeathInfo)
        {
            // A death, whoever caused it: IsPlayerDead is bInitialized && !bDBNO, and a player who went down first has
            // the knock's bDBNO in what the fields started from.
            DeathInfo.bDBNO = false;
            if (FDeathInfo::HasDeathCircumstance() && Natives::AFortPlayerPawnAthena_GetDeathCircumstance)
            {
                DeathInfo.DeathCircumstance = Natives::AFortPlayerPawnAthena_GetDeathCircumstance(Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(DeadPawn)));
            }
            AthenaState->SetDeathInfo(DeathInfo);
            const FVector DeathLocation = FDeathInfo::HasDeathLocation() ? static_cast<FVector>(DeathInfo.DeathLocation) : FVector(0.0f, 0.0f, 0.0f);
            UE_LOG(LogFortDeath, Log, TEXT("OnPawnDied: %s's death is stored by the player state's own store (cause %d, at (%.0f, %.0f, %.0f), circumstance %d)"), *GetName(),
                static_cast<int32_t>(DeathCause), DeathLocation.X, DeathLocation.Y, DeathLocation.Z, FDeathInfo::HasDeathCircumstance() ? static_cast<int32_t>(DeathInfo.DeathCircumstance) : 0);
        }
    }

    UWorld* World = GetWorld();
    if (Role != ROLE_Authority || !World)
    {
        return;
    }
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this);
    // A playlist that brings its players back (4.2's Playground, #308): the pawn's death handler (4.2 0xA81B50, 4.5
    // 0xA9B930, after this one) asks the game mode the same, with the death cause set above, and has
    // RespawnPlayerAfterDeath run once the death penalty's respawn countdown is over. The player stays in the match.
    const bool bRespawning = GameMode && AthenaState && GameMode->IsRespawningAllowed(AthenaState);
    APawn* KillerPawn = Instigator ? Instigator->GetPawn() : nullptr;
    // A player who will respawn spectates meanwhile too, as one who is out does (#426: on Epic's 4.5 Playground the
    // camera went to another player until the respawn). The game's respawn is built for it: before its timer the pawn's
    // death handler (4.5 0xA9B930) sets the controller's bPlayerIsWaiting (+0x510 bit 0x10; the engine's "waiting to
    // respawn, only valid in Spectating state"), which is what lets RespawnPlayerAfterDeath's ServerRestartPlayer (4.5
    // 0xAA8410 calls slot 0x7E0, APlayerController's 0x23A7400) restart a controller SpectateOnDeath has put in the
    // Spectating state; without it that restart only takes an Inactive one. So the first spectate comes five seconds
    // after the death, before a 10 s countdown (5.0's Anthem_DeathPenalty, AthenaRespawnDelayOverTime) is over, and not
    // at all where the countdown is shorter (6.01 and later, 3 s): the respawn stops it (StopSpectatingOnDeath).
    if (GameMode && GameMode->bAllowSpectateAfterDeath)
    {
        if (Instigator)
        {
            PlayerToSpectateOnDeath = KillerPawn;
        }
        // 1.9: GetWorldTimerManager().SetTimer(Handle, this, &AFortPlayerControllerAthena::SpectateOnDeath, 2, true, 5).
        // Bound to the controller, so the timer manager drops it with the controller.
        World->GetTimerManager().SetTimer(SpectateOnDeathTimerHandleOf(this), FTimerDelegate::CreateUObject(this, &AFortPlayerControllerAthena::SpectateOnDeath),
            SpectateRate, true, SpectateFirstDelay);
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
    if (bRespawning)
    {
        // The countdown the client shows (the death report's ServerTimeForRespawn, which AFortPlayerControllerZone's
        // PrepareDeathReport filled above), against the spectating's first five seconds.
        const float RespawnIn = Member_PlayerDeathReport.IsAvailable() && FFortPlayerDeathReport::HasServerTimeForRespawn()
            ? static_cast<FFortPlayerDeathReport&>(PlayerDeathReport).ServerTimeForRespawn - World->GetTimeSeconds() : -1.0f;
        UE_LOG(LogFortDeath, Log, TEXT("OnPawnDied: %s respawns, the playlist brings its players back (the death report's countdown: %.1f s; spectating from %.0f s)"), *GetName(),
            RespawnIn, GameMode->bAllowSpectateAfterDeath ? SpectateFirstDelay : -1.0f);
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
    const bool bSquadPlay = IsSquadPlay(GameMode, GameState);
    if (bSquadPlay && Natives::AFortPlayerControllerAthena_UpdateTeamMembersIndex >= 0)
    {
        CallVirtual<void>(this, Natives::AFortPlayerControllerAthena_UpdateTeamMembersIndex);
    }
    if (IsTeamOut())
    {
        Natives::AFortPlayerControllerAthena_SetTeamPlace(this, GameState->TeamsLeft);
    }
    // Battle Royale's player bots (CoreV2 #233) are in the match's counts but not among its alive players: while any is
    // left, the game's solo win (the last of the alive players, once the count it had was more than 1) mustn't run, so
    // its count before is held at 0; the bots' manager puts the counts right and checks the win after.
    // Its team win too (with bSquadPlay: TeamsLeft at 1 or fewer once the team this player leaves without players is
    // taken off it, 3.5.2 0x9FA050, 3.3 0x9FD930), whose count has the bots' teams in it: in 50v50 a player's death took
    // it from 2 to 1 and ended the match with both sides' bots still in. It is held above 1 the same way.
    FCoreAthenaBotManager* BotManager = FCoreAthenaBotManager::Get();
    if (BotManager && BotManager->ShouldHoldNativeWinCheck())
    {
        GameState->PlayersLeft = 0;
        if (bSquadPlay)
        {
            GameState->TeamsLeft = std::max(static_cast<int32_t>(GameState->TeamsLeft), 1) + 2;
        }
    }
    // From 4.4 the finisher's pawn, weapon and the death cause go on to the win (ClientNotifyWon's parameters).
    APawn* FinisherPawn = Instigator && Instigator != this ? KillerPawn : nullptr;
    AFortPawn* FinisherFortPawn = Cast<AFortPawn>(reinterpret_cast<UObject*>(FinisherPawn));
    AFortWeapon* FinishingWeapon = FinisherFortPawn ? static_cast<AFortWeapon*>(FinisherFortPawn->CurrentWeapon) : nullptr;
    UFortWeaponItemDefinition* FinishingWeaponData = FinishingWeapon ? static_cast<UFortWeaponItemDefinition*>(FinishingWeapon->WeaponData) : nullptr;
    Natives::AFortGameModeAthena_RemoveFromAlivePlayers(GameMode, this, Instigator ? static_cast<APlayerState*>(Instigator->PlayerState) : nullptr, FinisherPawn,
        FinishingWeaponData, DeathCause, false);
    if (BotManager)
    {
        BotManager->OnPlayerRemoved(AthenaStateOf(Instigator), FinisherPawn, FinishingWeaponData, DeathCause);
    }
    UE_LOG(LogFortDeath, Log, TEXT("OnPawnDied: %s is out (place %d, %d players and %d teams left)"), *GetName(), static_cast<int32_t>(AthenaState->Place), static_cast<int32_t>(GameState->PlayersLeft), static_cast<int32_t>(GameState->TeamsLeft));
    GameMode->SendOutliveStatEvents(DeadPawn);
    // The player's own end-of-match stats, their team still in or not: the screen shows them under "Team Still Alive"
    // until the team's place and the rewards come with the match's results (#403). The tick after, as the game mode's
    // results are looked at, once the death's handling is over.
    World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(static_cast<AFortPlayerController*>(this), &AFortPlayerController::SendMatchStatsForPlayer));

    Reflection::FParams Params(StaticFunction_ClientFinishedInteractionInZone());
    Params.Call(this);
#endif
}
