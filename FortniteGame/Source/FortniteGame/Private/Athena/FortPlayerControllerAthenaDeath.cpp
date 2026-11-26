#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/AI/CoreAthenaBotManager.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlaylistAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortTeamInfo.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Packed.h"
#include "Pawns/FortPawn.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "Weapons/FortWeapon.h"
#include "UObject/WeakObjectPtr.h"

#include <cstring>
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
    // 7.20 (Release-7.20, CL-4727874): a bleed-out's elimination distance is the knock's (OnPawnDied).
    constexpr uint32_t BleedOutKeepsKnockDistanceChangelist = 4727874;

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

    // The members of FGameplayEventData the earned elimination's event fills in, in place in SendGameplayEventToActor's
    // parameter (the engine owns the struct's handles).
    struct FEarnedEliminationEventData
    {
        DefineUnrealStruct(FGameplayEventData, GameplayAbilities)
        DefineUProperty(AActor*, Instigator)
        DefineUProperty(AActor*, Target)
        DefineUProperty(FGameplayTagContainer, InstigatorTags)
        DefineUProperty(FGameplayTagContainer, TargetTags)
    };
}

void AFortPlayerControllerAthena::SendEarnedEliminationEvent(AFortPlayerStateAthena* KillerPlayerState, AFortPawn* EliminatedPawn)
{
#if WITH_SERVER_CODE
    // Core's: no build has the sender left (7.40, 8.00 and 8.20 register Event.EarnedElimination in FFortGameplayTags,
    // +0xA0/+0xA8, and nothing reads it), so it went with the stripped death code. GAS_DefaultPlayer's
    // GA_SiphonEffect_OnKillGrant (from 7.40) triggers on it: the eliminator from the event's Instigator
    // (GetFortPlayerControllerFromActor, then its pawn), and on 7.40 the TargetRequiredTags Class.IsCommando, which an
    // event's activation checks against the payload's TargetTags. AthenaGameData's Default.Siphon rows decide what it
    // gives (50 health or shield, the 50 of each material), the hotfix's on the builds Epic turned them on by hotfix.
    static const FName EarnedEliminationTag(L"Event.EarnedElimination", FNAME_Find);
    AFortPlayerControllerAthena* KillerController = KillerPlayerState ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(KillerPlayerState->GetOwner())) : nullptr;
    AFortPawn* KillerPawn = KillerController ? static_cast<AFortPawn*>(KillerController->MyFortPawn) : nullptr;
    if (EarnedEliminationTag.IsNone() || !KillerPawn || KillerPawn->bTearOff)
    {
        return;
    }
    static UFunction* Send = FindObject<UFunction>(nullptr, L"/Script/GameplayAbilities.AbilitySystemBlueprintLibrary:SendGameplayEventToActor");
    UObject* Library = UAbilitySystemBlueprintLibrary::StaticClass() ? UAbilitySystemBlueprintLibrary::StaticClass()->GetDefaultObject() : nullptr;
    Reflection::FParams Event(Send, true);
    FEarnedEliminationEventData* Payload = Send ? static_cast<FEarnedEliminationEventData*>(Event.GetAddress(L"Payload")) : nullptr;
    if (!Library || !Payload || !Event.Set(L"Actor", static_cast<AActor*>(KillerPawn)) || !Event.Set(L"EventTag", FGameplayTag{ EarnedEliminationTag }))
    {
        static bool bWarned = false;
        if (!std::exchange(bWarned, true))
        {
            UE_LOG(LogFortDeath, Warning, TEXT("SendEarnedEliminationEvent: SendGameplayEventToActor isn't usable, so eliminations don't trigger Event.EarnedElimination (siphon)"));
        }
        return;
    }
    Payload->Instigator = KillerController;
    Payload->Target = EliminatedPawn;
    if (UAbilitySystemComponent* KillerAbilitySystem = KillerPawn->AbilitySystemComponent)
    {
        KillerAbilitySystem->GetOwnedGameplayTags(Payload->InstigatorTags);
    }
    if (UAbilitySystemComponent* EliminatedAbilitySystem = EliminatedPawn ? static_cast<UAbilitySystemComponent*>(EliminatedPawn->AbilitySystemComponent) : nullptr)
    {
        EliminatedAbilitySystem->GetOwnedGameplayTags(Payload->TargetTags);
    }
    Event.Call(Library);
    static bool bLogged = false;
    if (!std::exchange(bLogged, true))
    {
        UE_LOG(LogFortDeath, Log, TEXT("SendEarnedEliminationEvent: %s earned an elimination (Event.EarnedElimination on its pawn)"), *KillerController->GetName());
    }
#endif
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

// 7.00+ Creative's "Start or join a Creative server" (7.10's quest_br_fdf_play_creative_mode) counts
// complete_athena_creative_mode: an AthenaRank row whose context is Athena.Creative, Playlist_PlaygroundV2's
// GameplayTagContainer. AthenaRank is the event of a player's finished match, and 7.10 keeps no sender of it: none of
// its 16 SendStatEventWithTags call sites (each a call of the stripped nullsub) passes AthenaRank (0x1A). Battle
// Royale's are counted by Core-Backend from the match's results, which a Creative match never has (no place), so the
// event is sent here, as the player's match ends by their leaving the server, flushed while their controller is still
// there (AController::Destroyed's CleanupPlayerState destroys the player state before the controller goes).
void AFortGameModeAthena::SendCreativeRankStatEvent(AFortPlayerStateAthena* PlayerState)
{
#if WITH_SERVER_CODE
    if (!PlayerState || !AFortPlayerControllerAthena::IsCreativeMatch(this))
    {
        return;
    }
    const EFortQuestObjectiveStatEvent AthenaRank{ EFortQuestObjectiveStatEvent::StaticEnumInfo().FindValue(L"AthenaRank") };
    if (static_cast<uint8_t>(AthenaRank) == Reflection::InvalidEnumValue)
    {
        return;
    }
    AFortPlayerController* Controller = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(PlayerState->GetOwner()));
    UFortRegisteredPlayerInfo* PlayerInfo = Controller ? static_cast<UFortRegisteredPlayerInfo*>(Controller->MyPlayerInfo) : nullptr;
    UFortQuestManager* QuestManager = PlayerInfo ? PlayerInfo->GetAthenaQuestManager() : nullptr;
    if (!QuestManager)
    {
        Log::Info(L"Quests: a player left Creative without a quest manager, no AthenaRank event");
        return;
    }
    const FGameplayTagContainer NoTargetTags;
    FGameplayTagContainer SourceTags;
    FGameplayTagContainer ContextTags;
    QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
    QuestManager->SendStatEvent(AthenaRank, nullptr, NoTargetTags, SourceTags, ContextTags, 1, true);
#endif
}

void AFortPlayerControllerAthena::HookVTables()
{
#if WITH_SERVER_CODE
    HookQuickChatVTables();
    HookSuicideVTables();
    HookPlayerStateRpcVTables();

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

    const int32_t Index = Finder::StrippedAthenaPawnDiedIndex(MarkerComponentOffset());
    if (Index < 0)
    {
        // The build's own handler takes a player out of the match and checks its win (1.8's RemoveFromAlivePlayers,
        // 0x403060). Its StartEndGamePhase is still what the player bots' win ends the match with (CoreV2 #486).
        Finder::ResolveStartEndGamePhaseLive();
        return;
    }
    if (!Finder::ResolveAthenaPawnDiedLive(AFortPlayerStateAthena::PlaceOffset(), AFortPlayerStateAthena::SecondsAliveOffset(),
        AFortPlayerStateAthena::DeathInfoOffset() + FDeathInfo::DeathCauseOffset(), AFortPlayerStateAthena::DeathInfoOffset() + FDeathInfo::DBNOOffset(), TeamMembersOffset()))
    {
        Log::Error(L"Battle Royale: this build strips the pawn-death handler and its pieces were not found, so nobody who dies is out of the match");
        return;
    }
    SwapStripped(L"AFortPlayerControllerAthena::OnPawnDied", Index, Natives::AFortPlayerControllerZone_OnPawnDiedWithPawn ? reinterpret_cast<void*>(&OnPawnDiedWithPawnHK) : reinterpret_cast<void*>(&OnPawnDiedHK));
    // The player state's own store of a death, which the restored handler hands its FDeathInfo to (OnPawnDied). From
    // 6.01 only: every reader of a stored death was read in 6.01's and 6.02's images, no older build's, and bInitialized
    // (from 4.4) changes what each of them does. Older builds keep the report written field by field. 6.01 to 6.22 are
    // the builds whose game state reflects bAllowRespawning; 6.30 (CL-4573096) drops it and reflects the game state's
    // IsRespawningAllowed instead (AFortGameModeAthena::IsRespawningAllowed), and keeps every reader. Gated on the
    // first alone, 6.30 to 7.20 stored no death the game's way, so the volume manager's respawn place for a player
    // (7.10 0xDBAD80: the minigame of the volume the death is in, AFortMinigame::OverridePlayerSpawnLocation, whose
    // ChoosePlayerStart takes the player's latest Creative checkpoint, a spawn pad or the air) was never asked, and the
    // game mode's FinishRestartPlayer (7.10 0xC2AB10) never moved the respawned pawn (PongooDev/CoreV2#492).
    if ((AFortGameStateAthena::HasAllowRespawning() || AFortGameStateZone::HasIsRespawningAllowed()) && FDeathInfo::HasInitialized() && Natives::AFortPlayerStateAthena_ToDeathCause)
    {
        const ptrdiff_t DeathInfoOffset = AFortPlayerStateAthena::DeathInfoOffset();
        const ptrdiff_t DeathCircumstanceOffset = FDeathInfo::DeathCircumstanceOffset();
        Finder::ResolveSetDeathInfoLive(DeathInfoOffset, DeathInfoOffset + FDeathInfo::DeathCauseOffset(), DeathInfoOffset + FDeathInfo::InitializedOffset(),
            DeathCircumstanceOffset >= 0 ? DeathInfoOffset + DeathCircumstanceOffset : -1);
    }
#endif
}

void AFortPlayerControllerAthena::HookSuicideVTables()
{
#if WITH_SERVER_CODE
    // ServerSuicide is Suicide's RPC: AFortPlayerController::Suicide (an exec, BlueprintCallable; 7.40 0x1435A90) sends it
    // while the controller is in the Playing state. 7.40 (CL-5046157) gives this class its own _Implementation (slot
    // 0x26A), and it is the shared empty function (0x2B5120), so the self-destruct did nothing. 7.30 (CL-4834550) has no
    // override here: the class inherits AFortPlayerControllerZone's (0x144FF60), which runs AFortPlayerController's
    // (0x1417210: a pawn more than a second old suicides through AFortPawn's virtual) and then clears the death report's
    // respawn times. 7.40 keeps both (Zone's 0x1464AE0, which this class's parent AFortPlayerControllerPvP inherits, and
    // 0x142A750). Only where this class's slot is a stub and its parent's isn't.
    UFunction* const Function = AFortPlayerController::FindStaticFunction_ServerSuicide();
    const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
    UClass* const SuperClass = Index >= 0 ? StaticClass()->GetSuperClass() : nullptr;
    UObject* const SuperDefault = SuperClass ? SuperClass->GetDefaultObject(false) : nullptr;
    void* const Super = SuperDefault ? (*reinterpret_cast<void***>(SuperDefault))[Index] : nullptr;
    if (!Super || Finder::IsStubFunction(reinterpret_cast<uintptr_t>(Super)))
    {
        return;
    }

    int32_t Restored = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            ServerSuicide_ImplementationSuper = reinterpret_cast<decltype(ServerSuicide_ImplementationSuper)>(Super);
            Hooks::SwapVTable(L"AFortPlayerControllerAthena::ServerSuicide_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerSuicide_ImplementationHK));
            ++Restored;
        }
    }
    if (Restored > 0)
    {
        Log::Info(L"Finder: AFortPlayerControllerAthena::ServerSuicide_Implementation (slot 0x{:X}) is stripped, restored in {} vtables", Index, Restored);
    }
#endif
}

void AFortPlayerControllerAthena::ServerSuicide_ImplementationHK(AFortPlayerControllerAthena* This)
{
    This->ServerSuicide_Implementation();
}

void AFortPlayerControllerAthena::ServerSuicide_Implementation()
{
#if WITH_SERVER_CODE
    // Core's: the parent's, as 7.30's class inherited it; 7.40's own body is compiled out, so what it added is unknown.
    // The pawn's death then goes the way of any other (OnPawnDied, the death report, the respawn where the mode has one).
    static bool bLoggedOnce = false;
    if (!bLoggedOnce)
    {
        bLoggedOnce = true;
        UE_LOG(LogFortDeath, Log, TEXT("ServerSuicide: %s self-destructs (pawn %s)"), *GetName(), *GetNameSafe(static_cast<AFortPawn*>(MyFortPawn)));
    }
    ServerSuicide_ImplementationSuper(this);
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
    This->OnPawnDied(nullptr, Damage, DamageTags, Momentum, Instigator, DamageCauser, Downer);
}

void AFortPlayerControllerAthena::OnPawnDiedWithPawnHK(AFortPlayerControllerAthena* This, AFortPlayerPawn* DiedPawn, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer)
{
    This->OnPawnDied(DiedPawn, Damage, DamageTags, Momentum, Instigator, DamageCauser, Downer);
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
    // 7.40 (CL-5046157) drops the controller's TeamMembers: the team is then its player state's AFortTeamInfo, whose
    // TeamMembers are the team's controllers (AddToTeam adds each), read through their player states the same way.
    if (!HasTeamMembers())
    {
        AFortTeamInfo* Team = AthenaState && AFortTeamInfo::HasTeamMembers() ? static_cast<AFortTeamInfo*>(AthenaState->PlayerTeam) : nullptr;
        const TArray<AController*>* Controllers = Team ? &Team->GetMember_TeamMembers() : nullptr;
        for (int32_t MemberIndex = 0; Controllers && MemberIndex < Controllers->Num(); ++MemberIndex)
        {
            AController* Controller = (*Controllers)[MemberIndex];
            AFortPlayerStateAthena* Member = Controller ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState))) : nullptr;
            AFortPlayerPawn* MemberPawn = Member ? Member->GetCurrentPawn() : nullptr;
            if (MemberPawn && !MemberPawn->bTearOff)
            {
                return false;
            }
        }
        return true;
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

void AFortPlayerControllerAthena::OnPawnDied(AFortPlayerPawn* DiedPawn, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer)
{
    // 9.00 (0x1085440) first clears the dead pawn's timer (+0xC40) on the world's timer manager.
    if (UWorld* TimerWorld = DiedPawn && Natives::AFortPlayerPawn_PawnDiedTimerHandleOffset >= 0 ? GetWorld() : nullptr)
    {
        TimerWorld->GetTimerManager().ClearTimer(*reinterpret_cast<FTimerHandle*>(reinterpret_cast<uint8_t*>(DiedPawn) + Natives::AFortPlayerPawn_PawnDiedTimerHandleOffset));
    }
    // 1.9 0x42DB20. Zone's handler first: the death report and ClientOnPawnDied, the inventory drop and where the
    // controller stands (Core's, PawnDiedInventoryDropCall).
    if (Natives::AFortPlayerControllerZone_OnPawnDiedWithPawn)
    {
        Natives::AFortPlayerControllerZone_OnPawnDiedWithPawn(this, DiedPawn, Damage, DamageTags, Momentum, Instigator, DamageCauser, Downer);
    }
    else
    {
        Natives::AFortPlayerControllerZone_OnPawnDied(this, Damage, DamageTags, Momentum, Instigator, DamageCauser, Downer);
    }
    // 3.5 keeps this much of its own body (3.5.1 0xA27A00): the player's map marker goes on the server.
    if (Natives::AFortPlayerControllerAthena_RemoveMapCursor && Role == ROLE_Authority)
    {
        Natives::AFortPlayerControllerAthena_RemoveMapCursor(this);
    }
    // 7.00 (0xC5C980) makes the same two steps one by one: the ServerRemoveMapCursor RPC, then the player state's marker.
    else if (Natives::AFortPlayerControllerAthena_ServerRemoveMapCursor && Natives::AFortPlayerStateAthena_SetMapIndicatorPos && Role == ROLE_Authority)
    {
        Natives::AFortPlayerControllerAthena_ServerRemoveMapCursor(this);
        if (AFortPlayerStateAthena* MarkerState = AthenaStateOf(this))
        {
            Natives::AFortPlayerStateAthena_SetMapIndicatorPos(MarkerState, FVector2D(0.0f, 0.0f));
        }
    }
    // 8.00 (0xDBDEF0) asks the controller's MarkerComponent to take the player's map marker away.
    else if (Natives::UAthenaMarkerComponent_LocalRemoveMapMarker && Role == ROLE_Authority)
    {
        if (UAthenaMarkerComponent* Markers = MarkerComponent)
        {
            Natives::UAthenaMarkerComponent_LocalRemoveMapMarker(Markers);
        }
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
        // 0xAAE619; 5.10 0xC11510). From 7.20 (CL-4727874) a player who bleeds out keeps the knock's distance: "When a
        // player dies from Down-But-Not-Out bleed-out damage, the elimination distance is calculated from the original
        // down location rather than the distance when they are fully eliminated" (v7.20 notes). The knock (7.20's DBNO
        // handling 0xD986D0) stores it with bDBNO: the downer's pawn's GetDistanceTo the player (0xD98D41), or their
        // LastFallDistance (0xD98DE4), handed to the player state's store (0xD98DEA), which the fields here start from.
        // Before 7.20 the elimination took the distance at that moment, as this handler always did.
        const bool bBledOut = bSetDeathInfo && Packed::Changelist() >= BleedOutKeepsKnockDistanceChangelist && DeathCause == EDeathCause::GetDBNOTimeout()
            && FDeathInfo::HasInitialized() && StoredDeathInfo.bDBNO && StoredDeathInfo.bInitialized;
        if (FDeathInfo::HasDistance() && !bBledOut)
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
        // The downer (from 9.00), which only the knock stores (9.01's DBNO handling, 0x10BA540: the downer's player state, as
        // FinisherOrDowner). Wick's Bounty's CommonDeadPawn pays the dead player's tokens to it as the pawn's death is
        // broadcast, after this handler (0x10A9E30); without one it drops them as a pickup. Core's: an elimination stores
        // its eliminator there as the knock stores its downer (a knocked player's, the knock's), and a death nobody caused
        // none.
        if (FDeathInfo::HasDowner())
        {
            AController* const Eliminator = Downer ? Downer : Instigator;
            if (Eliminator)
            {
                DeathInfo.Downer = reinterpret_cast<AActor*>(AthenaStateOf(Eliminator));
            }
            else if (!DeadPawn || !DeadPawn->bIsDBNO)
            {
                DeathInfo.Downer = nullptr;
            }
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
            if (FDeathInfo::HasDeathTags())
            {
                // The stored info's tags are the damage's, which the store (8.30 0xF9DB30) copies into the player
                // state's; this raw copy only points at them for that copy and is never destroyed.
                std::memcpy(&static_cast<FGameplayTagContainer&>(DeathInfo.DeathTags), &DamageTags, sizeof(FGameplayTagContainer));
            }
            AthenaState->SetDeathInfo(DeathInfo);
            const FVector DeathLocation = FDeathInfo::HasDeathLocation() ? static_cast<FVector>(DeathInfo.DeathLocation) : FVector(0.0f, 0.0f, 0.0f);
            UE_LOG(LogFortDeath, Log, TEXT("OnPawnDied: %s's death is stored by the player state's own store (cause %d, at (%.0f, %.0f, %.0f), circumstance %d)"), *GetName(),
                static_cast<int32_t>(DeathCause), DeathLocation.X, DeathLocation.Y, DeathLocation.Z, FDeathInfo::HasDeathCircumstance() ? static_cast<int32_t>(DeathInfo.DeathCircumstance) : 0);
        }
    }

    UWorld* World = GetWorld();
    // 8.40 (0xF88F12) ends the handler with the respawn invalidation: when the game state lets the player respawn
    // (AFortGameStateZone::IsRespawningAllowed, which asks its mutators and the player's stored death), the controller's
    // timer (0xF97C10) that later checks whether the respawn is still allowed.
    if (Natives::AFortPlayerControllerAthena_StartRespawnInvalidationTimer && Natives::AFortGameStateZone_IsRespawningAllowedIndex >= 0 && World)
    {
        AFortGameStateAthena* InvalidationGameState = Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState()));
        AFortPlayerState* InvalidationPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
        if (InvalidationGameState && CallVirtual<bool>(InvalidationGameState, Natives::AFortGameStateZone_IsRespawningAllowedIndex, InvalidationPlayerState))
        {
            Natives::AFortPlayerControllerAthena_StartRespawnInvalidationTimer(this);
        }
    }
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
    // A death on 6.30's warmup island, which hurts (bAllowDamageInWarmup, #452): the game brings the player back there,
    // and the killer the death stored isn't an elimination in the match's results.
    if (GameState && AthenaState && GameState->GamePhase <= EAthenaGamePhase::GetWarmup() && AFortGameModeAthena::AllowsDamageInWarmup(GameMode))
    {
        GameMode->NoteWarmupDeath(AthenaState);
    }
    if (!GameState || !GameMode || GameState->GamePhase <= EAthenaGamePhase::GetWarmup() || !bMarkedAlive || !AthenaState)
    {
        return;
    }
    // 8.20's island games (#583): AFortMinigame::AddMinigamePlayer (0x13185B0) moves a player who joins a game under way
    // to spectate by killing their pawn as SilentSwitchingToSpectate, then sends ClientClearDeathNotification; the kill
    // feed calls it switching to spectate (KillFeedSwitchingToSpectate). It is no elimination: taking the player out of
    // the match here gave them Battle Royale's end (the match stats and its leave) while the island went on. They stay
    // in it and come back with the game's other players after the game (AFortMinigame::PostGameResetPlayers).
    // Only 8.20's EDeathCause has the value.
    static const uint8_t SilentSwitchingToSpectate = EDeathCause::StaticEnumInfo().FindValue(L"SilentSwitchingToSpectate");
    if (SilentSwitchingToSpectate != Reflection::InvalidEnumValue && DeathCause == SilentSwitchingToSpectate)
    {
        UE_LOG(LogFortDeath, Log, TEXT("OnPawnDied: %s switches to spectating the island's game, still in the match"), *GetName());
        return;
    }
    // The elimination is the eliminator's earned kill when they are of another team (the eliminator the death stored,
    // FinisherOrDowner). The game state's mutators hear of it (21.40's FortGameStateAthena_OnPostEarnedKill), which is how
    // 6.31's Team Rumble scores its teams. Its HUD and the game mode's own Tick (the team placements, the win at 100)
    // read that score. Before the player is taken out, so a win check that follows sees the score.
    AController* const Eliminator = Downer ? Downer : Instigator;
    AFortPlayerStateAthena* const EliminatorState = Eliminator && Eliminator != this ? AthenaStateOf(Eliminator) : nullptr;
    AFortPlayerStateAthena* const KillerState = EliminatorState && EliminatorState != AthenaState
        && static_cast<uint8_t>(EliminatorState->TeamIndex) != static_cast<uint8_t>(AthenaState->TeamIndex) ? EliminatorState : nullptr;
    if (KillerState)
    {
        GameState->OnPostEarnedKill(KillerState);
        SendEarnedEliminationEvent(KillerState, DeadPawn);
    }
    if (bRespawning)
    {
        // Still in the match: the elimination is the killer's, not the player's way out of it (Core's match results; a
        // bot that did it counts it too).
        GameMode->NoteRespawnDeath(AthenaState, KillerState);
        if (FCoreAthenaBotManager* BotManager = FCoreAthenaBotManager::Get(); BotManager && KillerState)
        {
            BotManager->OnRespawningPlayerKilled(KillerState);
        }
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
    const bool bTeamOut = IsTeamOut();
    if (bTeamOut)
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
    {
        // Endgame's win, which RemoveFromAlivePlayers asks the mutator for, counts the bots on each side (CoreV2 #653).
        const FCoreAthenaBotManager::FScopedTeamAliveBots TeamAliveBots(BotManager && BotManager->IsMutatorControlledMatch() ? BotManager : nullptr);
        Natives::AFortGameModeAthena_RemoveFromAlivePlayers(GameMode, this, Instigator ? static_cast<APlayerState*>(Instigator->PlayerState) : nullptr, FinisherPawn,
            FinishingWeaponData, DeathCause, false);
    }
    if (BotManager)
    {
        BotManager->OnPlayerRemoved(AthenaStateOf(Instigator), FinisherPawn, FinishingWeaponData, DeathCause);
    }
    UE_LOG(LogFortDeath, Log, TEXT("OnPawnDied: %s is out (place %d, %d players and %d teams left)"), *GetName(), static_cast<int32_t>(AthenaState->Place), static_cast<int32_t>(GameState->PlayersLeft), static_cast<int32_t>(GameState->TeamsLeft));
    // The team is out: the game mode queues it for removal from the server (AFortGameModeAthena::QueueTeamForServerRemoval,
    // 6.10 on). No build shows its caller (the call went with the server-only function). Core's: the moment a team is out
    // in the game's code is RemoveFromAlivePlayers taking its last player off the team's alive count (7.20 0xD14D3A,
    // which takes the team off TeamsLeft), which is intact code Core doesn't hook, so the restored handler that just
    // called it makes the call.
    if (bTeamOut && AFortGameModeAthena::HasForceKickAfterDeath())
    {
        GameMode->QueueTeamForServerRemoval(static_cast<uint8_t>(AthenaState->TeamIndex));
    }
    GameMode->SendOutliveStatEvents(DeadPawn);
    // The player's own end-of-match stats, their team still in or not: the screen shows them under "Team Still Alive"
    // until the team's place and the rewards come with the match's results (#403). The tick after, as the game mode's
    // results are looked at, once the death's handling is over.
    World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateUObject(static_cast<AFortPlayerController*>(this), &AFortPlayerController::SendMatchStatsForPlayer));

    Reflection::FParams Params(StaticFunction_ClientFinishedInteractionInZone());
    Params.Call(this);
#endif
}
