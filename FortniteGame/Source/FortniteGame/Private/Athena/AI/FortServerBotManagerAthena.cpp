#include "pch.h"
#include "Athena/AI/FortServerBotManagerAthena.h"
#include "AIController.h"
#include "Athena/AI/AthenaNavSystem.h"
#include "Building/BuildingContainer.h"
#include "Athena/FortAthenaAircraft.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Athena/FortPoiVolume.h"
#include "Athena/FortSafeZoneIndicator.h"
#include "Configuration.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/GameMode.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "GameThread.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace
{
    std::unique_ptr<UFortServerBotManagerAthena> Instance;

    // The bots each player eliminated (their match results add them), and the bot pawns the game itself sent a Kill
    // quest event for.
    std::vector<std::pair<TWeakObjectPtr<AFortPlayerStateAthena>, int32_t>> BotKills;
    std::vector<TWeakObjectPtr<UObject>> NativeKillTargets;

    // Core's bot names (8.30 names a bot from its customization loadout, else "Bot": AFortAthenaAIBotController's
    // 0xD49760; 21.40 picks from a name list, AssignDisplayName).
    const wchar_t* const BotNames[] = {
        L"Ace", L"Blaze", L"Bolt", L"Bramble", L"Breeze", L"Briar", L"Buckshot", L"Cinder", L"Clover", L"Comet",
        L"Copper", L"Coral", L"Crow", L"Dash", L"Dune", L"Echo", L"Ember", L"Falcon", L"Fern", L"Flint",
        L"Frost", L"Gale", L"Ghost", L"Glider", L"Granite", L"Hawk", L"Hazel", L"Hex", L"Indigo", L"Ivy",
        L"Jade", L"Jinx", L"Juniper", L"Kestrel", L"Kite", L"Lark", L"Lumen", L"Lynx", L"Maple", L"Marble",
        L"Marsh", L"Meteor", L"Mist", L"Moss", L"Nova", L"Oak", L"Onyx", L"Orbit", L"Otter", L"Pebble",
        L"Pepper", L"Pike", L"Pixel", L"Quartz", L"Quill", L"Rain", L"Raven", L"Reed", L"Ridge", L"Rook",
        L"Rust", L"Sable", L"Sage", L"Scout", L"Shadow", L"Shale", L"Slate", L"Sparrow", L"Spark", L"Spruce",
        L"Storm", L"Summit", L"Talon", L"Thistle", L"Thorn", L"Thunder", L"Tide", L"Timber", L"Torch", L"Tundra",
        L"Vapor", L"Vesper", L"Viper", L"Volt", L"Wander", L"Wasp", L"Willow", L"Wisp", L"Wren", L"Yarrow",
        L"Zephyr", L"Zinc", L"Bandit", L"Cobalt", L"Drift", L"Fable", L"Gravel", L"Harbor", L"Maverick", L"Puddle",
    };
    constexpr int32_t NumBotNames = static_cast<int32_t>(sizeof(BotNames) / sizeof(BotNames[0]));

    // Bots spawned per frame during the warmup, and how often the loot list is refreshed.
    constexpr int32_t BotsPerFrame = 3;
    constexpr float LootRefreshInterval = 3.0f;

    void WatchManager()
    {
        GameThread::Enqueue([]()
        {
            if (!Instance)
            {
                return;
            }
            if (!Instance->GetGameMode())
            {
                Log::Info(L"Battle Royale bots: the game mode is gone, the bot manager stops");
                Instance.reset();
                return;
            }
            Instance->Tick();
            WatchManager();
        });
    }

    FVector LocationOf(const AActor* Actor)
    {
        return Actor ? Actor->GetTransform().GetLocation() : FVector();
    }

    float RandomRange(float Min, float Max)
    {
        return Min + (Max - Min) * FMath::FRand();
    }

    AFortGameStateAthena* AthenaGameStateOf(AFortGameModeAthena* GameMode)
    {
        UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
        return World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    }

    AFortPlayerStateAthena* AthenaStateOf(AController* Controller)
    {
        return Controller ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState))) : nullptr;
    }

    UClass* RangedWeaponClass()
    {
        static UClass* Class = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortWeaponRangedItemDefinition");
        return Class;
    }
}

void UFortServerBotManagerAthena::Create(AFortGameModeAthena* GameMode)
{
#if WITH_SERVER_CODE
    if (!GameMode || (Instance && Instance->GetGameMode() == GameMode))
    {
        return;
    }
    const bool bWatching = Instance != nullptr;
    Instance.reset(new UFortServerBotManagerAthena(GameMode));
    BotKills.clear();
    NativeKillTargets.clear();
    if (!bWatching)
    {
        WatchManager();
    }
#endif
}

UFortServerBotManagerAthena* UFortServerBotManagerAthena::Get()
{
    return Instance.get();
}

UFortServerBotManagerAthena::UFortServerBotManagerAthena(AFortGameModeAthena* InGameMode)
    : GameMode(InGameMode)
{
}

bool UFortServerBotManagerAthena::HasNativePlayerBots()
{
    static const bool bNative = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortAthenaAIBotController") != nullptr;
    return bNative;
}

AFortGameModeAthena* UFortServerBotManagerAthena::GetGameMode() const
{
    return static_cast<AFortGameModeAthena*>(GameMode.Get());
}

void UFortServerBotManagerAthena::SetSettings(bool bInEnabled, const FFortAthenaBotSettings& InSettings)
{
    const bool bChanged = !bHasSettings || bInEnabled != bEnabled || InSettings.Count != Settings.Count || InSettings.Level != Settings.Level
        || InSettings.StartingHealth != Settings.StartingHealth || InSettings.StartingShield != Settings.StartingShield
        || InSettings.bEliminationsCountForQuests != Settings.bEliminationsCountForQuests;
    bHasSettings = true;
    if (!bChanged)
    {
        return;
    }
    // The match keeps the bots it has once the bus has left.
    if (bAircraftStarted)
    {
        Settings.bEliminationsCountForQuests = InSettings.bEliminationsCountForQuests;
        return;
    }
    bEnabled = bInEnabled;
    Settings = InSettings;
    if (bEnabled && HasNativePlayerBots())
    {
        Log::Info(L"Battle Royale bots: this build has Epic's own player bots, so \"Force player AIs\" (Core's backport) is ignored");
        bEnabled = false;
        return;
    }
    if (bEnabled)
    {
        Log::Info(L"Battle Royale bots: Force player AIs is on: up to {} bots, level {}, {:.0f} health, {:.0f} shield, eliminations {} for quests", Settings.Count, Settings.Level,
            Settings.StartingHealth, Settings.StartingShield, Settings.bEliminationsCountForQuests ? L"count" : L"don't count");
    }
    else
    {
        Log::Info(L"Battle Royale bots: Force player AIs is off");
    }
}

FAIBotSkills UFortServerBotManagerAthena::SkillsForLevel(int32_t Level)
{
    // Core's levels 🟡 (8.30's FAIBotSkills values don't ship): level 1 reacts slowly, shoots slowly and misses a lot;
    // level 10 reacts in a fifth of a second and aims within a few units at 10 m.
    const float T = std::clamp((static_cast<float>(Level) - 1.0f) / 9.0f, 0.0f, 1.0f);
    FAIBotSkills Skills;
    Skills.MaxTimeBetweenShots = FMath::Lerp(0.9f, 0.25f, T);
    Skills.DeviationTimeBetweenShots = FMath::Lerp(0.5f, 0.1f, T);
    Skills.MaxTrackingReactionTime = FMath::Lerp(1.2f, 0.2f, T);
    Skills.MaxTrackingInterpTime = FMath::Lerp(0.6f, 0.1f, T);
    Skills.MaxTrackingPredictionError = FMath::Lerp(250.0f, 30.0f, T);
    Skills.SightRange = FMath::Lerp(4000.0f, 9000.0f, T);
    Skills.AimErrorAt10m = FMath::Lerp(140.0f, 12.0f, T);
    return Skills;
}

int32_t UFortServerBotManagerAthena::GetAliveBotCount() const
{
    return static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [](const auto& Bot) { return Bot->IsAlive(); }));
}

AFortAthenaAIBotController* UFortServerBotManagerAthena::FindBot(const UObject* PawnOrPlayerState) const
{
    if (!PawnOrPlayerState)
    {
        return nullptr;
    }
    for (const auto& Bot : PlayerBots)
    {
        if (reinterpret_cast<const UObject*>(Bot->GetPawn()) == PawnOrPlayerState || reinterpret_cast<const UObject*>(Bot->GetPlayerState()) == PawnOrPlayerState
            || reinterpret_cast<const UObject*>(Bot->GetController()) == PawnOrPlayerState)
        {
            return Bot.get();
        }
    }
    return nullptr;
}

bool UFortServerBotManagerAthena::IsBotPawn(const UObject* Object)
{
    return Instance && Instance->FindBot(Object) != nullptr;
}

int32_t UFortServerBotManagerAthena::BotKillsOf(const AFortPlayerStateAthena* PlayerState)
{
    const auto Found = std::find_if(BotKills.begin(), BotKills.end(), [PlayerState](const auto& Entry) { return Entry.first.Get() == PlayerState; });
    return Found != BotKills.end() ? Found->second : 0;
}

bool UFortServerBotManagerAthena::ShouldCountQuestEvent(const UObject* Target)
{
    return !Instance || Instance->Settings.bEliminationsCountForQuests;
}

void UFortServerBotManagerAthena::NoteNativeKillEvent(const UObject* Target)
{
    std::erase_if(NativeKillTargets, [](const TWeakObjectPtr<UObject>& Weak) { return !Weak.Get(); });
    NativeKillTargets.push_back(TWeakObjectPtr<UObject>(const_cast<UObject*>(Target)));
}

bool UFortServerBotManagerAthena::ShouldHoldNativeWinCheck() const
{
    return HasAliveBots();
}

void UFortServerBotManagerAthena::OnPlayerRemoved(AFortPlayerStateAthena* KillerState)
{
    if (AFortAthenaAIBotController* Killer = FindBot(KillerState))
    {
        Killer->AddKill();
    }
    UpdateCounts();
    CheckWinConditionLastManStanding();
}

bool UFortServerBotManagerAthena::GetSafeZone(FVector& OutCenter, float& OutRadius) const
{
    AFortGameModeAthena* Athena = GetGameMode();
    AFortSafeZoneIndicator* Indicator = Athena ? static_cast<AFortSafeZoneIndicator*>(Athena->SafeZoneIndicator) : nullptr;
    if (!Indicator || !AFortSafeZoneIndicator::HasNextCenter())
    {
        return false;
    }
    OutCenter = Indicator->NextCenter;
    OutRadius = Indicator->NextRadius;
    return OutRadius > 0.0f;
}

bool UFortServerBotManagerAthena::GetCurrentSafeZone(FVector& OutCenter, float& OutRadius) const
{
    AFortGameModeAthena* Athena = GetGameMode();
    AFortSafeZoneIndicator* Indicator = Athena ? static_cast<AFortSafeZoneIndicator*>(Athena->SafeZoneIndicator) : nullptr;
    if (!Indicator || !AFortSafeZoneIndicator::FindStaticFunction_GetSafeZoneCenter())
    {
        return false;
    }
    OutCenter = Indicator->GetSafeZoneCenter();
    OutRadius = Indicator->Radius;
    return OutRadius > 0.0f;
}

float UFortServerBotManagerAthena::GetStormDamagePerSecond() const
{
    // Battle Royale's storm hurts 1 a second in its first circles, more later 🟡 (the game's GE_OutsideSafeZone
    // damage by phase isn't read here).
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t Phase = Athena ? static_cast<int32_t>(Athena->SafeZonePhase) : 0;
    return Phase <= 2 ? 1.0f : Phase <= 4 ? 2.0f : Phase <= 6 ? 5.0f : 10.0f;
}

bool UFortServerBotManagerAthena::FindGroundNear(const FVector& Around, float Radius, FVector& OutLocation) const
{
    AFortGameModeAthena* Athena = GetGameMode();
    for (int32_t Attempt = 0; Athena && Attempt < 6; ++Attempt)
    {
        const float Angle = RandomRange(0.0f, 6.2831853f);
        const float Distance = Radius * std::sqrt(FMath::FRand());
        const FVector Top(Around.X + std::cos(Angle) * Distance, Around.Y + std::sin(Angle) * Distance, 30000.0f);
        FVector Hit;
        if (UKismetSystemLibrary::LineTraceSingle(Athena, Top, FVector(Top.X, Top.Y, -5000.0f), 0, false, false, Hit))
        {
            OutLocation = Hit;
            return true;
        }
    }
    return false;
}

uint8_t UFortServerBotManagerAthena::TeamOfPawn(const APawn* Pawn) const
{
    AFortPlayerStateAthena* State = Pawn ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Pawn->PlayerState))) : nullptr;
    return State ? static_cast<uint8_t>(State->TeamIndex) : 0xFF;
}

void UFortServerBotManagerAthena::ForgetLoot(AFortPickup* Pickup)
{
    std::erase_if(Loot, [Pickup](const FFortAthenaBotLoot& Entry) { return Entry.Pickup.Get() == Pickup; });
}

std::set<uint8_t> UFortServerBotManagerAthena::HumanTeams() const
{
    std::set<uint8_t> Teams;
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (!GameState)
    {
        return Teams;
    }
    const TArray<APlayerState*>& PlayerArray = GameState->PlayerArray;
    for (APlayerState* PlayerState : PlayerArray)
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (AthenaState && !FindBot(AthenaState) && AthenaState->TeamIndex != EFortTeam::GetSpectator())
        {
            Teams.insert(static_cast<uint8_t>(AthenaState->TeamIndex));
        }
    }
    return Teams;
}

std::set<uint8_t> UFortServerBotManagerAthena::AliveTeams(bool bIncludeHumans) const
{
    std::set<uint8_t> Teams;
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive())
        {
            Teams.insert(Bot->GetTeam());
        }
    }
    AFortGameModeAthena* Athena = GetGameMode();
    if (bIncludeHumans && Athena)
    {
        const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
        for (int32_t Index = 0; Index < Alive.Num(); ++Index)
        {
            if (AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Alive[Index])))
            {
                Teams.insert(static_cast<uint8_t>(State->TeamIndex));
            }
        }
    }
    return Teams;
}

void UFortServerBotManagerAthena::Tick()
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!GameState || !World)
    {
        return;
    }
    const float Now = World->GetTimeSeconds();
    const float DeltaSeconds = LastTickTime < 0.0f ? 0.0f : std::max(Now - LastTickTime, 0.0f);
    LastTickTime = Now;
    // The server's frame (wall clock, the 30 Hz cap included) and the bots' share of it, for the summary.
    const auto FrameStart = std::chrono::steady_clock::now();
    if (LastFrameStart.time_since_epoch().count() != 0)
    {
        const double FrameMs = std::chrono::duration<double, std::milli>(FrameStart - LastFrameStart).count();
        FrameMsSum += FrameMs;
        FrameMsMax = std::max(FrameMsMax, FrameMs);
        ++FrameCount;
    }
    LastFrameStart = FrameStart;
    if (!bEnabled && PlayerBots.empty())
    {
        return;
    }

    const uint8_t Phase = GameState->GamePhase;
    if (!bAircraftStarted)
    {
        ReadPlaylistTeams();
    }
    StartBotMatchIfAsked();
    if (Phase != LastPhase)
    {
        LastPhase = Phase;
        Log::Info(L"Battle Royale bots: the match is in {} with {} bots", GameState->GamePhase.ToString(), GetAliveBotCount());
        if (Phase == EAthenaGamePhase::GetAircraft() && !bAircraftStarted)
        {
            OnAircraftPhaseStarted(Now);
        }
        if (Phase == EAthenaGamePhase::GetEndGame())
        {
            bMatchEnded = true;
        }
    }
    if (Phase == EAthenaGamePhase::GetWarmup())
    {
        TickWarmup(Now);
    }

    if (bHasNavigation)
    {
        UAthenaNavSystem::Tick(World);
    }
    RefreshEnemies();
    if (bAircraftStarted && Now >= NextLootRefresh)
    {
        NextLootRefresh = Now + LootRefreshInterval;
        RefreshLoot(Now);
    }
    if (bAircraftStarted && !bLandingAssigned && !bMatchEnded)
    {
        UpdateFlightPath(Now);
    }
    // Once the match has ended the bots stand still (the winners' screen is up).
    for (size_t Index = 0; Index < PlayerBots.size() && !bMatchEnded; ++Index)
    {
        PlayerBots[Index]->Tick(Now, DeltaSeconds);
    }
    if (bAircraftStarted && !bMatchEnded)
    {
        UpdateCounts();
        CheckWinConditionLastManStanding();
    }
    BotsMsSum += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - FrameStart).count();
    LogSummary(Now);
#endif
}

void UFortServerBotManagerAthena::StartBotMatchIfAsked()
{
#if WITH_SERVER_CODE
    // Testing only (-CoreBotMatch): a match of bots alone. Nobody's coming, so nothing assigns the match or starts its
    // warmup (HandleMatchHasStarted, 3.0 0x9AFB60, runs StartWarmupPhase once the match state goes InProgress); and at
    // the warmup's end UpdateWarmupPhase (0x9CC140) restarts the session when GetNumPlayers is 0 ("All players
    // disconnected during warmup phase"), so the bus is started a second early.
    if (!Configuration::Get().bBotMatch || !bEnabled)
    {
        return;
    }
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!GameState || !World)
    {
        return;
    }
    if (!bBotMatchStarted && GameState->GamePhase <= EAthenaGamePhase::GetSetup() && World->GetTimeSeconds() > 5.0f)
    {
        bBotMatchStarted = true;
        Log::Info(L"Battle Royale bots: -CoreBotMatch: starting a match of bots only");
        reinterpret_cast<AGameMode*>(Athena)->StartMatch();
    }
    if (!bBotMatchAircraftStarted && GameState->GamePhase == EAthenaGamePhase::GetWarmup() && AFortGameStateAthena::HasWarmupCountdown()
        && GameState->WarmupCountdownEndTime > 0.0f && World->GetTimeSeconds() >= GameState->WarmupCountdownEndTime - 2.0f && Athena->GetNumConnectedPlayers() == 0)
    {
        bBotMatchAircraftStarted = true;
        if (Natives::AFortGameModeAthena_StartAircraftPhase)
        {
            Log::Info(L"Battle Royale bots: -CoreBotMatch: the bus leaves with {} bots", GetAliveBotCount());
            Natives::AFortGameModeAthena_StartAircraftPhase(Athena);
        }
        else
        {
            Log::Error(L"Battle Royale bots: -CoreBotMatch: AFortGameModeAthena::StartAircraftPhase wasn't found, the bus can't leave");
        }
    }
#endif
}

void UFortServerBotManagerAthena::ReadPlaylistTeams()
{
    // The reservation beacon's teams, while it has them: a match whose players never came (the bots-only test match)
    // loses its beacon when its match starts, and the game session's own room is Save the World's 4.
    AFortGameModeAthena* Athena = GetGameMode();
    int32_t Teams = Athena ? Athena->GetNumTeams() : 0;
    int32_t TeamSize = Athena ? Athena->GetPlayersPerTeam() : 0;
    if (const int32_t Forced = Configuration::Get().BotTeamSize; Forced > 0 && Configuration::Get().bBotMatch)
    {
        // -CoreBotTeamSize (testing only, with -CoreBotMatch): duos or squads in a bots-only match.
        TeamSize = Forced;
        Teams = 100 / Forced;
    }
    if (Teams > 0 && TeamSize > 0 && (Teams != PlaylistTeams || TeamSize != PlaylistTeamSize))
    {
        PlaylistTeams = Teams;
        PlaylistTeamSize = TeamSize;
        Log::Info(L"Battle Royale bots: the playlist has {} teams of {}", Teams, TeamSize);
    }
}

int32_t UFortServerBotManagerAthena::MatchCapacity() const
{
    AFortGameModeAthena* Athena = GetGameMode();
    return PlaylistTeams > 0 ? PlaylistTeams * PlaylistTeamSize : Athena ? Athena->GetMatchCapacity() : 0;
}

int32_t UFortServerBotManagerAthena::WantedBotCount() const
{
    // bots = min(requested, MatchCapacity - connected players), re-checked until the bus leaves.
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena || !bEnabled)
    {
        return 0;
    }
    const int32_t Capacity = MatchCapacity();
    const int32_t Room = Capacity > 0 ? Capacity - Athena->GetNumConnectedPlayers() : Settings.Count;
    return std::clamp(std::min(Settings.Count, Room), 0, 100);
}

void UFortServerBotManagerAthena::TickWarmup(float Now)
{
#if WITH_SERVER_CODE
    if (!Finder::ResolveBotSkydiveLive(AFortPlayerPawnAthena::IsSkydivingOffset()) || !AAIController::StaticClass())
    {
        if (!bWarnedUnsupported && bEnabled)
        {
            bWarnedUnsupported = true;
            Log::Warning(L"Battle Royale: bots aren't supported on this build (no AAIController or AFortPlayerPawnAthena::BeginSkydiving), the match runs without them");
        }
        return;
    }
    const int32_t Wanted = WantedBotCount();
    if (Wanted != LastLoggedWanted)
    {
        AFortGameModeAthena* Athena = GetGameMode();
        Log::Info(L"Battle Royale bots: {} bots for the match (asked for {}, room for {} of {} with {} players connected)", Wanted, Settings.Count,
            Athena ? MatchCapacity() - Athena->GetNumConnectedPlayers() : 0, MatchCapacity(), Athena ? Athena->GetNumConnectedPlayers() : 0);
        LastLoggedWanted = Wanted;
    }
    const int32_t Alive = GetAliveBotCount();
    if (Alive > Wanted)
    {
        RemoveSurplusBots(Wanted);
    }
    else if (Alive < Wanted)
    {
        SpawnBots(Wanted, Now);
    }
#endif
}

void UFortServerBotManagerAthena::SpawnBots(int32_t Wanted, float Now)
{
    for (int32_t Spawned = 0; Spawned < BotsPerFrame && GetAliveBotCount() < Wanted && Now >= NextRegisterTime; ++Spawned)
    {
        if (!RegisterBot(Now))
        {
            // Try again in a while (no team free, a spawn failed).
            NextRegisterTime = Now + 2.0f;
            return;
        }
    }
}

void UFortServerBotManagerAthena::RemoveSurplusBots(int32_t Wanted)
{
    // The newest bots go first. A bot on a team a player has since joined (late joining) goes before them.
    const std::set<uint8_t> Humans = HumanTeams();
    std::stable_sort(PlayerBots.begin(), PlayerBots.end(), [&Humans](const auto& A, const auto& B)
    {
        const bool bAHuman = Humans.contains(A->GetTeam());
        const bool bBHuman = Humans.contains(B->GetTeam());
        if (bAHuman != bBHuman)
        {
            return !bAHuman;
        }
        return A->GetBotIndex() < B->GetBotIndex();
    });
    while (GetAliveBotCount() > Wanted && !PlayerBots.empty())
    {
        AFortAthenaAIBotController* Bot = PlayerBots.back().get();
        Log::Info(L"Battle Royale bots: {} leaves to make room (the match wants {} bots)", Bot->GetBotName(), Wanted);
        Bot->Destroy();
        PlayerBots.pop_back();
    }
}

uint8_t UFortServerBotManagerAthena::PlaceBotOnTeam()
{
    // 21.40's AFortGameModeAthena::PlaceBotOnTeam; 8.30's RegisterBot fills the playlist's teams in order, cycling
    // LastBotTeam. Core's bots play on teams of their own (the owner's choice): a team no player is on, from the last
    // (HumanPvP_Team100 on solo's 100 teams) down, while players' teams come from the first, filled to the playlist's
    // team size.
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t TeamSize = std::max(PlaylistTeamSize, 1);
    const int32_t NumTeams = PlaylistTeams > 0 ? std::min(PlaylistTeams, 100) : 100;
    const uint8_t FirstTeam = EFortTeam::GetHumanPvP_Team1();
    const std::set<uint8_t> Humans = HumanTeams();
    for (int32_t Index = NumTeams - 1; Index >= 0; --Index)
    {
        const uint8_t Team = static_cast<uint8_t>(FirstTeam + Index);
        if (Humans.contains(Team))
        {
            continue;
        }
        const int32_t OnTeam = static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [Team](const auto& Bot) { return Bot->IsAlive() && Bot->GetTeam() == Team; }));
        if (OnTeam < TeamSize)
        {
            return Team;
        }
    }
    return 0xFF;
}

bool UFortServerBotManagerAthena::RegisterBot(float Now)
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!World)
    {
        return false;
    }
    const uint8_t Team = PlaceBotOnTeam();
    if (Team == 0xFF)
    {
        if (!bLoggedCap)
        {
            bLoggedCap = true;
            Log::Warning(L"Battle Royale bots: no team is free for another bot");
        }
        return false;
    }

    if (WarmupSpots.empty())
    {
        UClass* WarmupStartClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortPlayerStartWarmup");
        for (AActor* Start : UGameplayStatics::GetAllActorsOfClass(Athena, WarmupStartClass))
        {
            if (Start)
            {
                WarmupSpots.push_back(LocationOf(Start));
            }
        }
        Log::Info(L"Battle Royale bots: {} warmup spots", WarmupSpots.size());
        if (WarmupSpots.empty())
        {
            return false;
        }
    }

    // A stock AAIController with a player state of the game mode's class (bWantsPlayerState, read as the controller
    // finishes spawning: AAIController::PostInitializeComponents' InitPlayerState), as 8.30's bot controller has.
    FTransform SpawnTransform;
    SpawnTransform.Rotation.W = 1.0f;
    AAIController* Controller = Cast<AAIController>(UGameplayStatics::BeginDeferredActorSpawnFromClass(Athena, AAIController::StaticClass(), SpawnTransform,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn));
    if (!Controller)
    {
        Log::Error(L"Battle Royale bots: couldn't spawn an AIController");
        return false;
    }
    Controller->bWantsPlayerState = true;
    Controller->bStopAILogicOnUnposses = false;
    UGameplayStatics::FinishSpawningActor(Controller, SpawnTransform);
    AFortPlayerStateAthena* PlayerState = AthenaStateOf(Controller);
    if (!PlayerState)
    {
        Log::Error(L"Battle Royale bots: {} has no Athena player state ({}), so it can't play", Controller->GetName().ToStdString(),
            Controller->PlayerState ? static_cast<APlayerState*>(Controller->PlayerState)->GetClass()->GetName().ToStdString() : std::wstring(L"none"));
        Controller->K2_DestroyActor();
        return false;
    }

    const int32_t BotIndex = NextBotIndex++;
    auto Bot = std::make_unique<AFortAthenaAIBotController>(this, Controller, BotIndex, Team);
    const std::wstring Name = BotIndex < NumBotNames ? std::wstring(BotNames[BotIndex]) : std::format(L"{}{}", BotNames[BotIndex % NumBotNames], BotIndex / NumBotNames + 1);
    Bot->AssignDisplayName(Name);
    Bot->InitializeSkills(Settings.Level);
    const FVector& Spot = WarmupSpots[static_cast<size_t>(FMath::Rand()) % WarmupSpots.size()];
    if (!Bot->SpawnPawnAt(FVector(Spot.X + RandomRange(-150.0f, 150.0f), Spot.Y + RandomRange(-150.0f, 150.0f), Spot.Z + 100.0f), RandomRange(0.0f, 360.0f), false))
    {
        Bot->Destroy();
        return false;
    }
    AddToAliveBots(Bot.get());
    PlayerBots.push_back(std::move(Bot));
    return true;
#else
    return false;
#endif
}

void UFortServerBotManagerAthena::AddToAliveBots(AFortAthenaAIBotController* Bot)
{
    AFortPlayerStateAthena* State = Bot->GetPlayerState();
    Log::Info(L"Battle Royale bots: {} joins on {} ({}, {})", Bot->GetBotName(), State ? static_cast<EFortTeam>(State->TeamIndex).ToString() : std::wstring(L"no team"),
        Bot->GetController() ? Bot->GetController()->GetName().ToStdString() : std::wstring(), Bot->GetPawn() ? Bot->GetPawn()->GetName().ToStdString() : std::wstring(L"no pawn"));
}

void UFortServerBotManagerAthena::OnAircraftPhaseStarted(float Now)
{
#if WITH_SERVER_CODE
    bAircraftStarted = true;
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    AFortAthenaAircraft* Aircraft = nullptr;
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(Athena, AFortAthenaAircraft::StaticClass()))
    {
        Aircraft = Aircraft ? Aircraft : Cast<AFortAthenaAircraft>(Actor);
    }
    float DropStart = Now + 5.0f;
    float DropEnd = Now + 60.0f;
    if (Aircraft && AFortAthenaAircraft::HasDropTimes() && Aircraft->DropEndTime > Aircraft->DropStartTime)
    {
        DropStart = Aircraft->DropStartTime;
        DropEnd = Aircraft->DropEndTime;
    }
    DropStartTime = DropStart;
    DropEndTime = DropEnd;
    CurrentAircraft = FWeakObjectPtr(reinterpret_cast<UObject*>(Aircraft));
    if (!Aircraft)
    {
        Log::Warning(L"Battle Royale bots: no aircraft found, the bots drop from where they are");
    }
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive())
        {
            // 21.40's JumpOffBus picks its moment along the path; Core's spread the bots over the drop zone, most of them
            // in its first two thirds.
            const float T = std::pow(FMath::FRand(), 1.4f);
            Bot->EnterAircraft(DropStart + 1.0f + (DropEnd - DropStart - 3.0f) * T, DropEnd - 1.0f);
        }
    }
    BotsAtStart = GetAliveBotCount();
    HumansAtStart = Athena ? Athena->AlivePlayers.Num() : 0;
    // The match's starting players (the count the players outlived is worked out from, copied from PlayersLeft as the
    // aircraft starts) and its players and teams left count the bots.
    if (GameState && Natives::AFortGameStateAthena_StartingPlayers > 0)
    {
        int32_t& StartingPlayers = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(GameState) + Natives::AFortGameStateAthena_StartingPlayers);
        StartingPlayers = HumansAtStart + BotsAtStart;
    }
    UpdateCounts();
    Log::Info(L"Battle Royale bots: the bus leaves with {} players and {} bots (drop zone {:.0f} s to {:.0f} s from now)", HumansAtStart, BotsAtStart, DropStart - Now, DropEnd - Now);
    if (BotsAtStart > 0)
    {
        RefreshLoot(Now);
        GatherContainers();
        GatherLandingSpots();
        InitializeNavigation();
    }
#endif
}

void UFortServerBotManagerAthena::GatherLandingSpots()
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_FreeFalling lands at a POI (_SelectNextPOI). 3.x's named locations are its
    // AFortPoiVolumes tagged Athena.Location.POI.* (Core gives the shapeless ones a box, FortPoiVolume.cpp); the loot
    // elsewhere is in clusters of chests (houses and farms), which count as spots too.
    LandingSpots.clear();
    const auto LootAround = [this](const FVector& Center, float Radius)
    {
        float Value = 0.0f;
        for (const FFortAthenaBotContainer& Container : Containers)
        {
            const float Dx = Container.Location.X - Center.X;
            const float Dy = Container.Location.Y - Center.Y;
            Value += Dx * Dx + Dy * Dy < Radius * Radius ? (Container.bChest ? 1.0f : 0.3f) : 0.0f;
        }
        for (const FFortAthenaBotLoot& Entry : Loot)
        {
            const float Dx = Entry.Location.X - Center.X;
            const float Dy = Entry.Location.Y - Center.Y;
            Value += Entry.bWeapon && Dx * Dx + Dy * Dy < Radius * Radius ? 0.3f : 0.0f;
        }
        return Value;
    };
    size_t Named = 0;
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(GetGameMode(), AFortPoiVolume::StaticClass()))
    {
        AFortPoiVolume* Volume = static_cast<AFortPoiVolume*>(Actor);
        std::wstring Name;
        for (const FGameplayTag& Tag : Volume->LocationTags.GameplayTags)
        {
            const std::wstring TagName = Tag.TagName.ToString().ToStdString();
            if (TagName.rfind(L"Athena.Location.POI.", 0) == 0)
            {
                Name = TagName.substr(20);
            }
        }
        if (Name.empty())
        {
            continue;
        }
        const FBox Box = Volume->GetFallbackBox();
        const FVector Center = Box.IsValid ? Box.GetCenter() : LocationOf(Volume);
        const float Radius = Box.IsValid ? std::clamp(std::max(Box.GetExtent().X, Box.GetExtent().Y), 4000.0f, 15000.0f) : 8000.0f;
        if (std::fabs(Center.X) < 1.0f && std::fabs(Center.Y) < 1.0f)
        {
            continue;
        }
        const float Value = LootAround(Center, Radius);
        if (Value >= 2.0f)
        {
            LandingSpots.push_back({ Center, Radius, Name, Value, 0 });
            ++Named;
        }
    }
    // Clusters of chests away from the named locations: a 60 m grid, three chests or more to a cell.
    std::map<std::pair<int32_t, int32_t>, std::vector<FVector>> Cells;
    for (const FFortAthenaBotContainer& Container : Containers)
    {
        if (Container.bChest)
        {
            Cells[{ static_cast<int32_t>(std::floor(Container.Location.X / 6000.0f)), static_cast<int32_t>(std::floor(Container.Location.Y / 6000.0f)) }].push_back(Container.Location);
        }
    }
    for (const auto& [Cell, Chests] : Cells)
    {
        if (Chests.size() < 3)
        {
            continue;
        }
        FVector Center;
        for (const FVector& Chest : Chests)
        {
            Center = Center + Chest * (1.0f / static_cast<float>(Chests.size()));
        }
        bool bNearNamed = false;
        for (size_t Index = 0; Index < Named; ++Index)
        {
            const FFortAthenaBotLandingSpot& Spot = LandingSpots[Index];
            bNearNamed = bNearNamed || (Spot.Location.X - Center.X) * (Spot.Location.X - Center.X) + (Spot.Location.Y - Center.Y) * (Spot.Location.Y - Center.Y) < Spot.Radius * Spot.Radius;
        }
        if (!bNearNamed)
        {
            LandingSpots.push_back({ Center, 4500.0f, std::format(L"chests at ({:.0f}, {:.0f})", Center.X, Center.Y), LootAround(Center, 4500.0f), 0 });
        }
    }
    std::sort(LandingSpots.begin(), LandingSpots.end(), [](const FFortAthenaBotLandingSpot& A, const FFortAthenaBotLandingSpot& B) { return A.Loot > B.Loot; });
    std::wstring Top;
    for (size_t Index = 0; Index < LandingSpots.size() && Index < 8; ++Index)
    {
        Top += std::format(L"{}{} ({:.0f})", Index ? L", " : L"", LandingSpots[Index].Name, LandingSpots[Index].Loot);
    }
    Log::Info(L"Battle Royale bots: {} landing spots, {} of them named locations; the richest: {}", LandingSpots.size(), Named, Top);
#endif
}

void UFortServerBotManagerAthena::UpdateFlightPath(float Now)
{
#if WITH_SERVER_CODE
    // The bus flies a straight line at a steady speed: two of its locations a moment apart give its path.
    AActor* Aircraft = GetAircraft();
    if (!Aircraft || LandingSpots.empty())
    {
        bLandingAssigned = true;
        Log::Info(L"Battle Royale bots: no {}, the bots drop near the bus's path", Aircraft ? L"landing spots" : L"aircraft");
        return;
    }
    const FVector Location = LocationOf(Aircraft);
    if (!bHasFlightSample)
    {
        bHasFlightSample = true;
        FlightSample = Location;
        FlightSampleTime = Now;
        return;
    }
    const float Moved = std::sqrt((Location.X - FlightSample.X) * (Location.X - FlightSample.X) + (Location.Y - FlightSample.Y) * (Location.Y - FlightSample.Y));
    if (Moved < 2000.0f || Now - FlightSampleTime < 0.5f)
    {
        // The bus waiting on its start: sampled again from where it is.
        if (Now - FlightSampleTime > 2.0f && Moved < 100.0f)
        {
            FlightSample = Location;
            FlightSampleTime = Now;
        }
        return;
    }
    FlightSpeed = Moved / (Now - FlightSampleTime);
    FlightDirection = FVector((Location.X - FlightSample.X) / Moved, (Location.Y - FlightSample.Y) / Moved, 0.0f);
    FlightSample = Location;
    FlightSampleTime = Now;
    bLandingAssigned = true;
    Log::Info(L"Battle Royale bots: the bus flies at {:.0f} u/s heading ({:.2f}, {:.2f})", FlightSpeed, FlightDirection.X, FlightDirection.Y);
    AssignLandingSpots(Now);
#endif
}

void UFortServerBotManagerAthena::AssignLandingSpots(float Now)
{
#if WITH_SERVER_CODE
    // Each spot's moment: when the bus passes closest to it, within the drop zone. It is reachable from there when a
    // skydive covers the distance (Core's 🟡: 700 m; a bot glides ~12 m/s through a 60-80 s fall).
    constexpr float MaxReach = 70000.0f;
    struct FCandidate
    {
        size_t Spot;
        float Time;
        float Reach;
    };
    std::vector<FCandidate> Candidates;
    const float Earliest = std::max(DropStartTime + 1.0f, Now + 0.5f);
    const float Latest = DropEndTime - 1.0f;
    for (size_t Index = 0; Index < LandingSpots.size() && Latest > Earliest; ++Index)
    {
        const FVector& Spot = LandingSpots[Index].Location;
        const float Along = (Spot.X - FlightSample.X) * FlightDirection.X + (Spot.Y - FlightSample.Y) * FlightDirection.Y;
        const float Time = std::clamp(FlightSampleTime + Along / FlightSpeed, Earliest, Latest);
        const float Travelled = (Time - FlightSampleTime) * FlightSpeed;
        const float JumpX = FlightSample.X + FlightDirection.X * Travelled;
        const float JumpY = FlightSample.Y + FlightDirection.Y * Travelled;
        const float Reach = std::sqrt((Spot.X - JumpX) * (Spot.X - JumpX) + (Spot.Y - JumpY) * (Spot.Y - JumpY));
        if (Reach <= MaxReach)
        {
            Candidates.push_back({ Index, Time, Reach });
        }
    }
    if (Candidates.empty())
    {
        Log::Info(L"Battle Royale bots: no landing spot in reach of the bus's path, the bots drop near it");
        return;
    }
    // Spread: a spot's loot is shared by the bots sent there (its weight falls with the square of their number), with a
    // little chance in it, and a far spot counts a little less. -CoreBotHotDrop sends everyone to the richest one.
    const bool bHotDrop = Configuration::Get().bBotHotDrop;
    int32_t Assigned = 0;
    // A team lands together (duos and squads): its first bot picks the spot and the jump, its teammates follow it out.
    std::map<uint8_t, std::pair<const FCandidate*, float>> TeamSpots;
    for (const auto& Bot : PlayerBots)
    {
        if (!Bot->IsAlive() || Bot->GetState() != EFortAthenaAIBotState::InAircraft)
        {
            continue;
        }
        if (const auto Chosen = TeamSpots.find(Bot->GetTeam()); Chosen != TeamSpots.end())
        {
            const FFortAthenaBotLandingSpot& Spot = LandingSpots[Chosen->second.first->Spot];
            Bot->SetLandingSpot(Spot.Location, Spot.Radius, std::clamp(Chosen->second.second + RandomRange(0.1f, 0.6f), Earliest, Latest));
            ++Assigned;
            continue;
        }
        const FCandidate* Best = nullptr;
        float BestScore = -1.0f;
        for (const FCandidate& Candidate : Candidates)
        {
            const FFortAthenaBotLandingSpot& Spot = LandingSpots[Candidate.Spot];
            const float Shared = (1.0f + static_cast<float>(Spot.Bots)) * (1.0f + static_cast<float>(Spot.Bots));
            const float Score = bHotDrop ? Spot.Loot : Spot.Loot * RandomRange(0.7f, 1.3f) * (1.0f - 0.4f * Candidate.Reach / MaxReach) / Shared;
            if (Score > BestScore)
            {
                BestScore = Score;
                Best = &Candidate;
            }
        }
        FFortAthenaBotLandingSpot& Spot = LandingSpots[Best->Spot];
        ++Spot.Bots;
        const float JumpAt = std::clamp(Best->Time + RandomRange(-1.5f, 1.5f), Earliest, Latest);
        Bot->SetLandingSpot(Spot.Location, Spot.Radius, JumpAt);
        if (PlaylistTeamSize > 1)
        {
            TeamSpots[Bot->GetTeam()] = { Best, JumpAt };
        }
        ++Assigned;
    }
    std::wstring Used;
    int32_t SpotsUsed = 0;
    for (const FFortAthenaBotLandingSpot& Spot : LandingSpots)
    {
        if (Spot.Bots > 0)
        {
            Used += std::format(L"{}{} x{}", SpotsUsed++ ? L", " : L"", Spot.Name, Spot.Bots);
        }
    }
    Log::Info(L"Battle Royale bots: {} bots land at {} of {} reachable spots: {}", Assigned, SpotsUsed, Candidates.size(), Used);
#endif
}

void UFortServerBotManagerAthena::GatherContainers()
{
#if WITH_SERVER_CODE
    // Battle Royale's chests and ammo boxes (the tiered containers a player searches; not the floor loot spawners).
    Containers.clear();
    int32_t Chests = 0;
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(GetGameMode(), ABuildingContainer::StaticClass()))
    {
        ABuildingContainer* Container = Cast<ABuildingContainer>(Actor);
        const std::wstring ClassName = Container ? reinterpret_cast<UObject*>(Container)->GetClass()->GetName().ToStdString() : std::wstring();
        const bool bChest = ClassName.find(L"Chest") != std::wstring::npos;
        const bool bAmmo = ClassName.find(L"Ammo") != std::wstring::npos;
        if (!Container || Container->IsPendingKill() || Container->bDestroyed || (!bChest && !bAmmo) || ClassName.find(L"FloorLoot") != std::wstring::npos)
        {
            continue;
        }
        Containers.push_back({ TWeakObjectPtr<ABuildingContainer>(Container), LocationOf(Container), bChest });
        Chests += bChest ? 1 : 0;
    }
    Log::Info(L"Battle Royale bots: {} chests and {} ammo boxes to search", Chests, Containers.size() - static_cast<size_t>(Chests));
#endif
}

void UFortServerBotManagerAthena::OnContainerSearched(ABuildingContainer* Container)
{
    std::erase_if(Containers, [Container](const FFortAthenaBotContainer& Entry) { return Entry.Container.Get() == Container; });
    // Its loot is on the floor now: the bots see it on the next tick.
    NextLootRefresh = 0.0f;
}

void UFortServerBotManagerAthena::InitializeNavigation()
{
#if WITH_SERVER_CODE
    // The island: everything the bots go to (the loot and the containers), with room around it.
    FBox Island;
    for (const FFortAthenaBotLoot& Entry : Loot)
    {
        Island += Entry.Location;
    }
    for (const FFortAthenaBotContainer& Entry : Containers)
    {
        Island += Entry.Location;
    }
    if (!Island.IsValid)
    {
        return;
    }
    Island.Min = Island.Min - FVector(5000.0f, 5000.0f, 3000.0f);
    Island.Max = Island.Max + FVector(5000.0f, 5000.0f, 6000.0f);
    AFortGameModeAthena* Athena = GetGameMode();
    bHasNavigation = !Configuration::Get().bBotNoNavigation && UAthenaNavSystem::InitializeForBots(Athena ? Athena->GetWorld() : nullptr, Island);
#endif
}

void UFortServerBotManagerAthena::UpdateCounts()
{
    // 21.40 counts bots in the game state's players and teams left (AddToAliveBots / RemoveFromAliveBots); 3.0's game
    // mode only counts its alive players, so the bots are added to its numbers.
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (!GameState || !bAircraftStarted || BotsAtStart == 0)
    {
        return;
    }
    const int32_t PlayersLeft = Athena->AlivePlayers.Num() + GetAliveBotCount();
    const int32_t TeamsLeft = static_cast<int32_t>(AliveTeams(true).size());
    if (GameState->PlayersLeft != PlayersLeft || GameState->TeamsLeft != TeamsLeft)
    {
        GameState->PlayersLeft = PlayersLeft;
        GameState->TeamsLeft = TeamsLeft;
        GameState->ForceNetUpdate();
    }
}

void UFortServerBotManagerAthena::RemoveFromAliveBots(AFortAthenaAIBotController* Bot, AFortPlayerStateAthena* KillerState)
{
#if WITH_SERVER_CODE
    // 21.40's AFortGameModeAthena::RemoveFromAliveBots. The team's place is the teams left when its last player is out
    // (counted before this bot is).
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t TeamsBefore = static_cast<int32_t>(AliveTeams(true).size());
    AFortPlayerStateAthena* State = Bot->GetPlayerState();
    AFortAthenaAIBotController* KillerBot = FindBot(KillerState);
    if (KillerBot && KillerBot != Bot)
    {
        KillerBot->AddKill();
    }
    AFortPlayerControllerAthena* KillerPlayer = KillerState && !KillerBot ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(KillerState->GetOwner())) : nullptr;
    if (KillerPlayer)
    {
        const auto Found = std::find_if(BotKills.begin(), BotKills.end(), [KillerState](const auto& Entry) { return Entry.first.Get() == KillerState; });
        if (Found != BotKills.end())
        {
            ++Found->second;
        }
        else
        {
            BotKills.emplace_back(TWeakObjectPtr<AFortPlayerStateAthena>(KillerState), 1);
        }
        KillerState->ClientReportKill(State);
        // The player's elimination count now (the HUD's, OnRep_Kills; the match results set it again from the same
        // counts once a second), and their team's.
        if (AFortPlayerStateAthena::CountsKills())
        {
            KillerState->Kills = KillerState->Kills + 1;
            KillerState->ForceNetUpdate();
        }
        else if (AFortPlayerStateAthena::HasKillScore())
        {
            KillerState->SetKillScore(KillerState->GetKills() + 1);
        }
        UWorld* KillWorld = Athena ? Athena->GetWorld() : nullptr;
        AGameStateBase* KillGameState = KillWorld ? KillWorld->GetGameState() : nullptr;
        if (AFortPlayerStateAthena::HasTeamKillScore() && KillGameState)
        {
            const uint8_t KillerTeam = static_cast<uint8_t>(KillerState->TeamIndex);
            for (APlayerState* Other : KillGameState->PlayerArray)
            {
                AFortPlayerStateAthena* Mate = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(Other));
                if (Mate && static_cast<uint8_t>(Mate->TeamIndex) == KillerTeam)
                {
                    Mate->TeamKillScore = Mate->TeamKillScore + 1;
                }
            }
        }

        // Quests: the game's own Kill event (if it sent one for this pawn) went through UFortQuestManager already; with
        // the setting on, one is sent for a kill the game didn't report.
        UObject* Target = reinterpret_cast<UObject*>(Bot->GetPawn());
        const bool bNativeEvent = std::any_of(NativeKillTargets.begin(), NativeKillTargets.end(), [Target](const TWeakObjectPtr<UObject>& Weak) { return Weak.Get() == Target; });
        UFortRegisteredPlayerInfo* PlayerInfo = static_cast<UFortRegisteredPlayerInfo*>(KillerPlayer->MyPlayerInfo);
        UFortQuestManager* QuestManager = PlayerInfo ? PlayerInfo->GetAthenaQuestManager() : nullptr;
        if (Settings.bEliminationsCountForQuests && !bNativeEvent && QuestManager)
        {
            FGameplayTagContainer SourceTags;
            FGameplayTagContainer ContextTags;
            QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
            const FGameplayTagContainer TargetTags;
            QuestManager->SendStatEvent(EFortQuestObjectiveStatEvent::GetKill(), Target, TargetTags, SourceTags, ContextTags, 1, false);
        }
        Log::Info(L"Battle Royale bots: {} eliminated {} ({} bots now; quests: {})", KillerState->GetPlayerName().ToStdString(), Bot->GetBotName(), BotKillsOf(KillerState),
            !Settings.bEliminationsCountForQuests ? L"off" : bNativeEvent ? L"the game's event" : QuestManager ? L"Core's event" : L"no quest manager");
    }

    const bool bTeamOut = std::none_of(PlayerBots.begin(), PlayerBots.end(), [Bot](const auto& Other) { return Other.get() != Bot && Other->IsAlive() && Other->GetTeam() == Bot->GetTeam(); });
    Bot->OnPossesedPawnDied(KillerState);
    if (bTeamOut && bAircraftStarted)
    {
        for (const auto& Member : PlayerBots)
        {
            AFortPlayerStateAthena* MemberState = Member->GetTeam() == Bot->GetTeam() ? Member->GetPlayerState() : nullptr;
            if (MemberState)
            {
                MemberState->Place = TeamsBefore;
                MemberState->ForceNetUpdate();
            }
        }
    }
    Log::Info(L"Battle Royale bots: {} is out{} ({} bots left)", Bot->GetBotName(), bTeamOut && bAircraftStarted ? std::format(L", place {}", TeamsBefore) : std::wstring(), GetAliveBotCount());
    if (Athena)
    {
        UpdateCounts();
        CheckWinConditionLastManStanding();
    }
#endif
}

void UFortServerBotManagerAthena::ApplyBotDamage(AFortAthenaAIBotController* Attacker, AFortPawn* Victim, float Damage, UFortWeaponItemDefinition* Weapon)
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!Attacker || !Victim || !World || Victim->bIsDying || Victim->IsPendingKill() || Damage <= 0.0f)
    {
        return;
    }
    const float Now = World->GetTimeSeconds();
    NoteBotHit(Attacker, Victim);

    const float Shield = Victim->GetShield();
    const float ToShield = std::min(Shield, Damage);
    if (ToShield > 0.0f)
    {
        Victim->SetShield(Shield - ToShield);
    }
    const float Health = Victim->GetHealth() - (Damage - ToShield);
    if (Health > 0.5f)
    {
        Victim->SetHealth(Health);
        return;
    }
    // Squads: down, not out, while a teammate stands (the next hit on a downed bot finishes it).
    if (AFortAthenaAIBotController* VictimBot = FindBot(reinterpret_cast<const UObject*>(Victim)); VictimBot && VictimBot->CanGoDBNO())
    {
        Log::Info(L"Battle Royale bots: {} knocks {} down with {}", Attacker->GetBotName(), VictimBot->GetBotName(), Weapon ? Weapon->GetName().ToStdString() : std::wstring(L"the harvesting tool"));
        VictimBot->EnterDBNO(Attacker->GetPlayerState(), Now);
        return;
    }
    // The elimination: the death's damage tag is the weapon's own (the kill feed's weapon), the killer the bot.
    FGameplayTag DeathReason{ FName(L"Gameplay.Damage.Physical", FNAME_Find) };
    if (Weapon)
    {
        const FGameplayTagContainer& Tags = Weapon->GameplayTags;
        for (const FGameplayTag& Tag : Tags.GameplayTags)
        {
            DeathReason = Tag;
            break;
        }
    }
    Log::Info(L"Battle Royale bots: {} eliminates {} with {}", Attacker->GetBotName(), Victim->GetName().ToStdString(), Weapon ? Weapon->GetName().ToStdString() : std::wstring(L"the harvesting tool"));
    Victim->SetHealth(1.0f);
    Victim->ForceKill(DeathReason, reinterpret_cast<AController*>(Attacker->GetController()), reinterpret_cast<AActor*>(Attacker->GetPawn()));
#endif
}

bool UFortServerBotManagerAthena::UsesGameDamage(const AFortPawn* Victim) const
{
    return Victim && (!IsBotPawn(reinterpret_cast<const UObject*>(Victim)) || GetTeamSize() <= 1);
}

void UFortServerBotManagerAthena::NoteBotHit(AFortAthenaAIBotController* Attacker, AFortPawn* Victim)
{
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!Attacker || !Victim || !World)
    {
        return;
    }
    const float Now = World->GetTimeSeconds();
    std::erase_if(BotHits, [Now, Victim](const FBotHit& Hit) { return !Hit.Victim.Get() || Hit.Victim.Get() == Victim || Now - Hit.Time > 15.0f; });
    BotHits.push_back({ TWeakObjectPtr<AFortPawn>(Victim), TWeakObjectPtr<AFortPlayerStateAthena>(Attacker->GetPlayerState()), Now });
}

std::vector<AFortAthenaAIBotController*> UFortServerBotManagerAthena::TeammatesOf(const AFortAthenaAIBotController* Bot) const
{
    std::vector<AFortAthenaAIBotController*> Teammates;
    for (const auto& Other : PlayerBots)
    {
        if (Other.get() != Bot && Other->IsAlive() && Other->GetTeam() == Bot->GetTeam())
        {
            Teammates.push_back(Other.get());
        }
    }
    return Teammates;
}

AFortPlayerStateAthena* UFortServerBotManagerAthena::LastBotAttackerOf(const AFortPawn* Pawn) const
{
    for (auto It = BotHits.rbegin(); It != BotHits.rend(); ++It)
    {
        if (It->Victim.Get() == Pawn)
        {
            return It->Attacker.Get();
        }
    }
    return nullptr;
}

void UFortServerBotManagerAthena::CheckWinConditionLastManStanding()
{
#if WITH_SERVER_CODE
    // 21.40's AFortGameModeAthena::CheckWinConditionLastManStanding, for a match with bots in it: the game's own check
    // only sees its alive players. A team is left alone: its players win (the game's StartEndGamePhase), or, a bots'
    // team, it takes place 1 and the match ends. Bots never keep a match going: once its players are all out, it ends.
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (bMatchEnded || !bAircraftStarted || BotsAtStart == 0 || !GameState || GameState->GamePhase >= EAthenaGamePhase::GetEndGame()
        || !Natives::AFortGameModeAthena_StartEndGamePhase)
    {
        return;
    }
    const std::set<uint8_t> Teams = AliveTeams(true);
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    const int32_t HumansLeft = Alive.Num();
    if (Teams.size() <= 1)
    {
        bMatchEnded = true;
        if (HumansLeft > 0)
        {
            AFortPlayerControllerAthena* Winner = Alive[0];
            Log::Info(L"Battle Royale bots: {} is the last team standing, it wins", Winner ? Winner->GetName().ToStdString() : std::wstring());
            Natives::AFortGameModeAthena_StartEndGamePhase(Athena, Winner);
            return;
        }
        for (const auto& Bot : PlayerBots)
        {
            if (Bot->IsAlive())
            {
                if (AFortPlayerStateAthena* State = Bot->GetPlayerState())
                {
                    State->Place = 1;
                    State->ForceNetUpdate();
                }
                Log::Info(L"Battle Royale bots: {} wins the match (a bots' team)", Bot->GetBotName());
            }
        }
        Natives::AFortGameModeAthena_StartEndGamePhase(Athena, nullptr);
        return;
    }
    if (HumansAtStart > 0 && HumansLeft == 0)
    {
        bMatchEnded = true;
        Log::Info(L"Battle Royale bots: every player is out with {} bots left, so the match ends", GetAliveBotCount());
        Natives::AFortGameModeAthena_StartEndGamePhase(Athena, nullptr);
    }
#endif
}

void UFortServerBotManagerAthena::RefreshEnemies()
{
    EnemyPawns.clear();
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena)
    {
        return;
    }
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive() && Bot->GetPawn())
        {
            EnemyPawns.emplace_back(reinterpret_cast<AFortPawn*>(Bot->GetPawn()));
        }
    }
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPawn* Pawn = Player ? Cast<AFortPawn>(reinterpret_cast<UObject*>(Player->GetPawn())) : nullptr;
        if (Pawn)
        {
            EnemyPawns.emplace_back(Pawn);
        }
    }
}

void UFortServerBotManagerAthena::RefreshLoot(float Now)
{
#if WITH_SERVER_CODE
    Loot.clear();
    AFortGameModeAthena* Athena = GetGameMode();
    UClass* RangedClass = RangedWeaponClass();
    for (AActor* Actor : UGameplayStatics::GetAllActorsOfClass(Athena, AFortPickup::StaticClass()))
    {
        AFortPickup* Pickup = Cast<AFortPickup>(Actor);
        if (!Pickup || Pickup->bPickedUp || Pickup->IsPendingKill())
        {
            continue;
        }
        FFortItemEntry& Entry = Pickup->PrimaryPickupItemEntry;
        UFortItemDefinition* Definition = Entry.ItemDefinition;
        UFortWeaponItemDefinition* Weapon = Cast<UFortWeaponItemDefinition>(Definition);
        // A gun: a ranged weapon whose ammo is an ammo item (consumables, such as the shield potion, are ranged weapons
        // that are their own ammo; grenades have none).
        bool bWeapon = false;
        if (Weapon && RangedClass && reinterpret_cast<UObject*>(Weapon)->IsA(RangedClass))
        {
            auto [Found, bNew] = GunDefinitions.try_emplace(Weapon, false);
            if (bNew)
            {
                UFortWorldItemDefinition* AmmoDefinition = static_cast<TAssetPtr<UFortWorldItemDefinition>&>(Weapon->AmmoData).LoadSynchronous();
                Found->second = AmmoDefinition && Cast<UFortAmmoItemDefinition>(AmmoDefinition) != nullptr;
            }
            bWeapon = Found->second;
        }
        const bool bAmmo = Cast<UFortAmmoItemDefinition>(Definition) != nullptr;
        // A bandage, medkit, shield potion or Chug Jug (21.40's _Loot takes healing items too).
        const bool bConsumable = !bWeapon && !bAmmo && Weapon && AFortAthenaAIBotController::GetConsumableInfo(Definition) != nullptr;
        if (bWeapon || bAmmo || bConsumable)
        {
            Loot.push_back({ TWeakObjectPtr<AFortPickup>(Pickup), LocationOf(Pickup), bWeapon, bConsumable });
        }
    }
#endif
}

void UFortServerBotManagerAthena::LogSummary(float Now)
{
    if (Now < NextSummaryLog || PlayerBots.empty())
    {
        return;
    }
    NextSummaryLog = Now + 15.0f;
    int32_t ByState[5] = {};
    int32_t OnNavMesh = 0;
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    for (const auto& Bot : PlayerBots)
    {
        ++ByState[static_cast<int32_t>(Bot->GetState())];
        if (bHasNavigation && Bot->GetState() == EFortAthenaAIBotState::Landed && Bot->GetPawn() && UAthenaNavSystem::IsOnNavMesh(World, LocationOf(reinterpret_cast<const AActor*>(Bot->GetPawn()))))
        {
            ++OnNavMesh;
        }
    }
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    Log::Info(L"Battle Royale bots: {} in the warmup, {} in the aircraft, {} skydiving, {} landed ({} on the navmesh), {} out; {} loot pickups seen; players left {}, teams left {}; "
        L"frame {:.1f} ms average, {:.1f} ms max, bots {:.2f} ms a frame",
        ByState[0], ByState[1], ByState[2], ByState[3], OnNavMesh, ByState[4], Loot.size(), GameState ? static_cast<int32_t>(GameState->PlayersLeft) : 0, GameState ? static_cast<int32_t>(GameState->TeamsLeft) : 0,
        FrameCount ? FrameMsSum / FrameCount : 0.0, FrameMsMax, FrameCount ? BotsMsSum / FrameCount : 0.0);
    FrameMsSum = 0.0;
    FrameMsMax = 0.0;
    BotsMsSum = 0.0;
    FrameCount = 0;
}
