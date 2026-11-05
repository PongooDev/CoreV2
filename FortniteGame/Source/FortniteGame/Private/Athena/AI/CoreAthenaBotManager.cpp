#include "pch.h"
#include "Athena/AI/CoreAthenaBotManager.h"
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
#include "GameplayTagContainer.h"
#include "Heroes/FortHeroType.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortCheatManager.h"
#include "Quests/FortQuestManager.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cmath>
#include <format>

namespace
{
    std::unique_ptr<FCoreAthenaBotManager> Instance;

    // The bots each player eliminated (their match results add them), and the bot pawns the game itself sent a Kill
    // quest event for.
    std::vector<std::pair<TWeakObjectPtr<AFortPlayerStateAthena>, int32_t>> BotKills;
    std::vector<TWeakObjectPtr<UObject>> NativeKillTargets;

    // Core's bot names: player-like tags (8.30 names a bot from its customization loadout, else "Bot": AFortAthenaAIBotController's
    // 0xD49760; 21.40 picks from a name list, AssignDisplayName). Each match deals them out shuffled, none twice.
    const wchar_t* const BotNames[] = {
        L"xSniperWolf", L"Kayzo_YT", L"NoScopeNate", L"ToxicTurtle77", L"GhostRider_99", L"Zyphr", L"Frostbyte_TTV", L"LilJimmy2009",
        L"SweatyMango", L"BuilderBob_42", L"Dr4gonSlay3r", L"CrankKing", L"NightOwlGG", L"Blitzkrieg_X", L"TacoTuesday88", L"PixelPanda",
        L"MrBushCamper", L"ShadowStrike21", L"Vortexx", L"JustAFloss", L"CaptainCrunchy", L"TheRealDefault", L"OmegaWolf_", L"HeadshotHarry",
        L"Sk8rBoi_14", L"CoolKid_Max", L"Lemonade_Jay", L"iiFlame", L"BananaBoat_7", L"KingKaiz", L"NoobMaster_69", L"Mystic_Ninja",
        L"Turbo_Tom", L"LlamaDrama22", L"xXSlayerXx", L"Raptor_GG", L"Ruby_Rose", L"EpicGamer2010", L"ChillVibes_", L"Moonwalker88",
        L"Crypt1c", L"ZeroGravity_", L"BigChungus_44", L"Fl1ckGod", L"SilentAssassin", L"SunnyDayz", L"Jaxon_Plays", L"OG_Tilted",
        L"Bacon_Bits99", L"WhyNoW1n", L"Sonic_Boom7", L"Kr4ken", L"FreshPrince_", L"GoldenGoose23", L"StormChaser_X", L"DabOnEm2018",
        L"Cactus_Jack11", L"Mav3rick_", L"PotatoAim", L"LuckyLoot", L"Nova_Strike", L"AquaMan_09", L"Glitchy_TV", L"DeltaForce_7",
        L"SpicyNugget", L"TiltedTom_", L"Wolfie_x0", L"ShotgunSally", L"Bl4zeRunner", L"FuzzyPickles", L"Vexxed", L"MrLoot_Llama",
        L"Cheeto_Dust4", L"Phantom_Ace", L"Ty_The_Guy", L"NinjaTurtl3", L"BlueBerry_Pie", L"Zapdos_99", L"ProBuilder_Z", L"SnipeyMcSnipe",
        L"Kenzo_YT", L"HypeTrain42", L"Rocket_Rex", L"IceColdKilla", L"Jumpman_23", L"Cosmic_Kitty", L"Dusty_Divot8", L"HungryHippo_",
        L"Echo_Six", L"Wildcat_Wes", L"Peely4Life", L"SlurpJuice_", L"Rookie_Ray", L"AimAssist_Andy", L"Groot_Gamer", L"Lazer_Tag88",
        L"CrispyChicken_", L"MidnightMoss", L"Viper_Venom", L"Daisy_Duke07", L"TheSaltyOne", L"Bolt_Action_Bri", L"Panda_Express", L"KiloWatt_",
        L"SweetChinMusic", L"Juno_x", L"RetailRowRat", L"xTrickShotx", L"Mango_Mike12", L"FlossBoss3000", L"Zeke_The_Freak", L"Pancake_Pete",
        L"Atomic_Annie", L"Shifty_Sam", L"CloudSurfer_", L"Tomatohead_Tim", L"BrickByBrick", L"Rage_Quit_Ray", L"Nebula_Nick", L"Kaylee_Plays",
    };
    constexpr int32_t NumBotNames = static_cast<int32_t>(sizeof(BotNames) / sizeof(BotNames[0]));

    // Bots spawned per frame during the warmup (one: each loads its loadout's cosmetics from the paks, which three at
    // once made a hitch), and how often the loot list is refreshed.
    constexpr int32_t BotsPerFrame = 1;
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
                BotKills.clear();
                NativeKillTargets.clear();
                UAthenaNavSystem::ResetForNewMatch();
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

    // Whether a player on Team (in Squad, when it isn't 0) is still in the match (the game mode's alive players).
    bool HasAlivePlayerOnTeam(AFortGameModeAthena* GameMode, uint8_t Team, uint8_t Squad = 0)
    {
        if (!GameMode)
        {
            return false;
        }
        const TArray<AFortPlayerControllerAthena*>& Alive = GameMode->AlivePlayers;
        for (int32_t Index = 0; Index < Alive.Num(); ++Index)
        {
            AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Alive[Index]));
            if (State && static_cast<uint8_t>(State->TeamIndex) == Team && (Squad == 0 || static_cast<uint8_t>(State->SquadId) == Squad))
            {
                return true;
            }
        }
        return false;
    }

    // An elimination for Team: each of its players' team eliminations (AFortPlayerStateAthena::TeamKillScore, the HUD's).
    void AddTeamKillScore(AFortGameModeAthena* GameMode, uint8_t Team)
    {
        UWorld* World = GameMode ? GameMode->GetWorld() : nullptr;
        AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
        if (!AFortPlayerStateAthena::HasTeamKillScore() || !GameState)
        {
            return;
        }
        for (APlayerState* Other : GameState->PlayerArray)
        {
            AFortPlayerStateAthena* Mate = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(Other));
            if (Mate && static_cast<uint8_t>(Mate->TeamIndex) == Team)
            {
                Mate->TeamKillScore = Mate->TeamKillScore + 1;
            }
        }
    }

    UClass* RangedWeaponClass()
    {
        static UClass* Class = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortWeaponRangedItemDefinition");
        return Class;
    }
}

void FCoreAthenaBotManager::Create(AFortGameModeAthena* GameMode)
{
#if WITH_SERVER_CODE
    if (!GameMode || (Instance && Instance->GetGameMode() == GameMode))
    {
        return;
    }
    const bool bWatching = Instance != nullptr;
    // A new game mode is a new world (the server reloads the map after a match): nothing of the last match's bots is
    // kept. Their controllers and pawns went with the old world; Core only ever holds them weakly.
    Instance.reset(new FCoreAthenaBotManager(GameMode));
    BotKills.clear();
    NativeKillTargets.clear();
    UAthenaNavSystem::ResetForNewMatch();
    FCoreAthenaBotController::ResetForNewMatch();
    if (!bWatching)
    {
        WatchManager();
    }
#endif
}

FCoreAthenaBotManager* FCoreAthenaBotManager::Get()
{
    return Instance.get();
}

FCoreAthenaBotManager::FCoreAthenaBotManager(AFortGameModeAthena* InGameMode)
    : GameMode(InGameMode)
{
}

bool FCoreAthenaBotManager::HasNativePlayerBots()
{
    // 8.x's own bot controller class (the backport's rename to Core names took this lookup with it: it looked for Core's
    // class name, which no build has, so a native-bot build would have run the backport too).
    static const bool bNative = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortAthenaAIBotController") != nullptr;
    return bNative;
}

AFortGameModeAthena* FCoreAthenaBotManager::GetGameMode() const
{
    return static_cast<AFortGameModeAthena*>(GameMode.Get());
}

void FCoreAthenaBotManager::SetSettings(bool bInEnabled, const FCoreAthenaBotSettings& InSettings)
{
    const bool bChanged = !bHasSettings || bInEnabled != bEnabled || InSettings.Count != Settings.Count || InSettings.Level != Settings.Level
        || InSettings.StartingHealth != Settings.StartingHealth || InSettings.StartingShield != Settings.StartingShield
        || InSettings.bEliminationsCountForQuests != Settings.bEliminationsCountForQuests || InSettings.BotFightChance != Settings.BotFightChance
        || InSettings.bFillPlayerTeams != Settings.bFillPlayerTeams || InSettings.NoFillAccounts != Settings.NoFillAccounts;
    bHasSettings = true;
    if (!bChanged)
    {
        return;
    }
    // The match keeps the bots it has once the bus has left; how they behave follows the panel.
    if (bAircraftStarted)
    {
        Settings.bEliminationsCountForQuests = InSettings.bEliminationsCountForQuests;
        Settings.BotFightChance = InSettings.BotFightChance;
        return;
    }
    bEnabled = bInEnabled;
    Settings = InSettings;
    if (bEnabled && HasNativePlayerBots())
    {
        // The boundary with 8.30's native bots: Force player AIs is the backport's alone, and players' teams on such a
        // build are filled by Epic's own system (its bot manager), which Core doesn't drive yet.
        Log::Info(L"Battle Royale bots: this build has Epic's own player bots, so \"Force player AIs\" (Core's backport) is ignored{}",
            Settings.bFillPlayerTeams ? L", and \"Fill players' teams with bots\" is for the native bots, which Core doesn't run yet" : L"");
        bEnabled = false;
        return;
    }
    if (bEnabled)
    {
        Log::Info(L"Battle Royale bots: Force player AIs is {}: up to {} bots; players' teams {}; level {}, {:.0f} health, {:.0f} shield, eliminations {} for quests, {}% bot fights",
            Settings.Count > 0 ? L"on" : L"off", Settings.Count,
            Settings.bFillPlayerTeams ? std::format(L"filled with bots ({} players without fill)", Settings.NoFillAccounts.size()) : std::wstring(L"not filled"), Settings.Level,
            Settings.StartingHealth, Settings.StartingShield, Settings.bEliminationsCountForQuests ? L"count" : L"don't count", Settings.BotFightChance);
    }
    else
    {
        Log::Info(L"Battle Royale bots: Force player AIs and Fill players' teams with bots are off");
    }
}

FCoreAIBotSkills FCoreAthenaBotManager::SkillsForLevel(int32_t Level)
{
    // Core's levels 🟡 (8.30's FAIBotSkills values and 21.40's aiming skill sets don't ship): level 1 reacts slowly,
    // shoots slowly and misses a lot; level 10 reacts in a quarter of a second and tracks closely. The default level 5
    // aims for a player's hit rate, a quarter to two fifths of its assault rifle shots at mid range (10-35 m; a
    // headless 3.1 match: 23%, 16 of 69, against strafing bots); testers found the old error, which shrank to nothing
    // up close and at high levels, an aimbot.
    const float T = std::clamp((static_cast<float>(Level) - 1.0f) / 9.0f, 0.0f, 1.0f);
    FCoreAIBotSkills Skills;
    Skills.MaxTimeBetweenShots = FMath::Lerp(0.9f, 0.25f, T);
    Skills.DeviationTimeBetweenShots = FMath::Lerp(0.5f, 0.1f, T);
    Skills.MaxTrackingReactionTime = FMath::Lerp(1.2f, 0.25f, T);
    Skills.MaxTrackingInterpTime = FMath::Lerp(0.25f, 0.05f, T);
    Skills.MaxTrackingPredictionError = FMath::Lerp(250.0f, 30.0f, T);
    Skills.SightRange = FMath::Lerp(4000.0f, 9000.0f, T);
    Skills.MaxTrackingOffsetError = FMath::Lerp(70.0f, 20.0f, T);
    Skills.MaxTrackingDistanceNearError = FMath::Lerp(35.0f, 8.0f, T);
    Skills.MaxTrackingDistanceFarError = FMath::Lerp(50.0f, 12.0f, T);
    return Skills;
}

int32_t FCoreAthenaBotManager::GetAliveBotCount() const
{
    return static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [](const auto& Bot) { return Bot->IsAlive(); }));
}

FCoreAthenaBotController* FCoreAthenaBotManager::FindBot(const UObject* PawnOrPlayerState) const
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

bool FCoreAthenaBotManager::IsBotPawn(const UObject* Object)
{
    return Instance && Instance->FindBot(Object) != nullptr;
}

int32_t FCoreAthenaBotManager::BotKillsOf(const AFortPlayerStateAthena* PlayerState)
{
    const auto Found = std::find_if(BotKills.begin(), BotKills.end(), [PlayerState](const auto& Entry) { return Entry.first.Get() == PlayerState; });
    return Found != BotKills.end() ? Found->second : 0;
}

bool FCoreAthenaBotManager::ShouldCountQuestEvent(const UObject* Target)
{
    return !Instance || Instance->Settings.bEliminationsCountForQuests;
}

void FCoreAthenaBotManager::NoteNativeKillEvent(const UObject* Target)
{
    std::erase_if(NativeKillTargets, [](const TWeakObjectPtr<UObject>& Weak) { return !Weak.Get(); });
    NativeKillTargets.push_back(TWeakObjectPtr<UObject>(const_cast<UObject*>(Target)));
}

bool FCoreAthenaBotManager::IsWarmup()
{
    return Instance && !Instance->bAircraftStarted;
}

namespace
{
    // An actor's own gameplay tags (IGameplayTagAssetInterface::GetOwnedGameplayTags, through its reflected function,
    // whose exec finds the actor's interface).
    void AppendOwnedGameplayTags(UObject* Actor, FGameplayTagContainer& Tags)
    {
        static UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/GameplayTags.GameplayTagAssetInterface:GetOwnedGameplayTags");
        Reflection::FParams Params(Function, true);
        if (Actor && Function && Params.Call(Actor))
        {
            Tags.AppendTags(Params.Take<FGameplayTagContainer>(L"TagContainer"));
        }
    }
}

void FCoreAthenaBotManager::AddKillEventTags(const UObject* Target, AController* Killer, FGameplayTagContainer& TargetTags, FGameplayTagContainer& SourceTags)
{
#if WITH_SERVER_CODE
    // 3.1's AFortGameModeZone::Killed (0xB5E890) sends the killer's Kill event with the dead pawn's own tags as the
    // target's and the killer's (their player state's hero, UFortPlayerController 0xE466C0) with their pawn's as the
    // source's. Battle Royale's kill objectives read both (AthenaObjectiveStatTable: killingblow_athena_player asks for
    // Homebase.Class in each, killingblow_athena_player_shotgun for Weapon.Ranged.shotgun in the source's too). A bot's
    // pawn has no hero of a player's: its class is its outfit's hero's (HID_*: Homebase.Class.IsCommando...), and the
    // weapon is the one the killer holds (its item's GameplayTags).
    FCoreAthenaBotController* Bot = Instance ? Instance->FindBot(Target) : nullptr;
    if (!Bot)
    {
        return;
    }
    AppendOwnedGameplayTags(reinterpret_cast<UObject*>(Bot->GetPawn()), TargetTags);
    UAthenaCharacterItemDefinition* Character = Cast<UAthenaCharacterItemDefinition>(Bot->GetLoadoutCharacter());
    if (UFortHeroType* Hero = Character ? static_cast<UFortHeroType*>(Character->HeroDefinition) : nullptr)
    {
        TargetTags.AppendTags(static_cast<FGameplayTagContainer&>(reinterpret_cast<UFortItemDefinition*>(Hero)->GameplayTags));
    }
    AFortPawn* KillerPawn = Killer ? Cast<AFortPawn>(reinterpret_cast<UObject*>(static_cast<APawn*>(Killer->Pawn))) : nullptr;
    AFortWeapon* Weapon = KillerPawn ? static_cast<AFortWeapon*>(KillerPawn->CurrentWeapon) : nullptr;
    if (UFortWeaponItemDefinition* WeaponData = Weapon ? static_cast<UFortWeaponItemDefinition*>(Weapon->WeaponData) : nullptr)
    {
        SourceTags.AppendTags(static_cast<FGameplayTagContainer&>(reinterpret_cast<UFortItemDefinition*>(WeaponData)->GameplayTags));
    }
#endif
}

bool FCoreAthenaBotManager::ShouldHoldNativeWinCheck() const
{
    return HasAliveBots();
}

void FCoreAthenaBotManager::OnPlayerRemoved(AFortPlayerStateAthena* KillerState)
{
    if (FCoreAthenaBotController* Killer = FindBot(KillerState))
    {
        Killer->AddKill();
    }
    UpdateCounts();
    CheckWinConditionLastManStanding();
}

bool FCoreAthenaBotManager::GetSafeZone(FVector& OutCenter, float& OutRadius) const
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

bool FCoreAthenaBotManager::GetCurrentSafeZone(FVector& OutCenter, float& OutRadius) const
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

float FCoreAthenaBotManager::GetStormDamagePerSecond() const
{
    // Battle Royale's storm hurts 1 a second in its first circles, more later 🟡 (the game's GE_OutsideSafeZone
    // damage by phase isn't read here).
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t Phase = Athena ? static_cast<int32_t>(Athena->SafeZonePhase) : 0;
    return Phase <= 2 ? 1.0f : Phase <= 4 ? 2.0f : Phase <= 6 ? 5.0f : 10.0f;
}

bool FCoreAthenaBotManager::FindGroundNear(const FVector& Around, float Radius, FVector& OutLocation) const
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

bool FCoreAthenaBotManager::GetWarmupSpot(FVector& OutLocation) const
{
    if (WarmupSpots.empty())
    {
        return false;
    }
    OutLocation = WarmupSpots[static_cast<size_t>(FMath::Rand()) % WarmupSpots.size()];
    return true;
}

uint8_t FCoreAthenaBotManager::TeamOfPawn(const APawn* Pawn) const
{
    AFortPlayerStateAthena* State = Pawn ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Pawn->PlayerState))) : nullptr;
    return State ? static_cast<uint8_t>(State->TeamIndex) : 0xFF;
}

void FCoreAthenaBotManager::ForgetLoot(AFortPickup* Pickup)
{
    std::erase_if(Loot, [Pickup](const FCoreAthenaBotLoot& Entry) { return Entry.Pickup.Get() == Pickup; });
}

std::set<uint8_t> FCoreAthenaBotManager::HumanTeams() const
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

std::set<uint8_t> FCoreAthenaBotManager::AliveTeams(bool bIncludeHumans) const
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

void FCoreAthenaBotManager::Tick()
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
        FrameSpikes += FrameMs >= 50.0 ? 1 : 0;
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
        UpdateCounts();
    }

    if (bHasNavigation)
    {
        UAthenaNavSystem::Tick(World);
    }
    RefreshEnemies();
    // The warmup island's weapons too, once the bots are in (they pick them up for a fight there).
    if ((bAircraftStarted || bNavigationTried) && Now >= NextLootRefresh)
    {
        NextLootRefresh = Now + LootRefreshInterval;
        const auto LootStart = std::chrono::steady_clock::now();
        RefreshLoot(Now);
        LootRefreshMsMax = std::max(LootRefreshMsMax, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - LootStart).count());
    }
    if (bAircraftStarted && !bLandingAssigned && !bMatchEnded)
    {
        UpdateFlightPath(Now);
    }
    if (!bMatchEnded)
    {
        UpdatePlayerTeamBots(Now);
    }
    // Once the match has ended the bots stand still (the winners' screen is up).
    for (size_t Index = 0; Index < PlayerBots.size() && !bMatchEnded; ++Index)
    {
        const auto BotStart = std::chrono::steady_clock::now();
        PlayerBots[Index]->Tick(Now, DeltaSeconds);
        if (const double BotMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - BotStart).count(); BotMs > BotTickMsMax)
        {
            BotTickMsMax = BotMs;
            SlowestBotTick = PlayerBots[Index]->GetBotName();
        }
    }
    if (bAircraftStarted && !bMatchEnded)
    {
        UpdateCounts();
        CheckWinConditionLastManStanding();
    }
    const double BotsMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - FrameStart).count();
    BotsMsSum += BotsMs;
    BotsMsMax = std::max(BotsMsMax, BotsMs);
    LogSummary(Now);
#endif
}

void FCoreAthenaBotManager::StartBotMatchIfAsked()
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

void FCoreAthenaBotManager::ReadPlaylistTeams()
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

int32_t FCoreAthenaBotManager::MatchCapacity() const
{
    // The playlist's most players, not its team slots: the beacon's NumTeams x NumPlayersPerTeam is 100 x 2 in duos and
    // 100 x 4 in squads (GetMaxTeamInfoForPlaylist), while every Battle Royale playlist of these seasons takes 100
    // players (FortPlaylistAthena MaxPlayers: solo, duos, squads and 50v50). Duos and squads with bots went past 100.
    constexpr int32_t PlaylistMaxPlayers = 100;
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t Slots = PlaylistTeams > 0 ? PlaylistTeams * PlaylistTeamSize : Athena ? Athena->GetMatchCapacity() : 0;
    return Slots > 0 ? std::min(Slots, PlaylistMaxPlayers) : 0;
}

int32_t FCoreAthenaBotManager::WantedBotCount(int32_t PlayerTeamBots) const
{
    // bots = min(requested, MaxPlayers - connected players), re-checked until the bus leaves. Bots play on teams of
    // their own, filled to the playlist's team size: when the room is what limits them, whole teams only, and never
    // more than the teams no player is on can hold. The bots in players' teams' empty places ("Fill players' teams with
    // bots") are counted first: they take their room from these, so the match stays at its size. A large team game
    // (Teams of 20, HasSquadsInTeams) has only the playlist's few teams, players' teams too: the bots fill the room left
    // on them, in squads (GetNextBotTeamAndSquadId).
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena || !bEnabled || Settings.Count <= 0)
    {
        return 0;
    }
    const int32_t Capacity = MatchCapacity();
    int32_t Room = Capacity > 0 ? Capacity - Athena->GetNumConnectedPlayers() - PlayerTeamBots : Settings.Count;
    const int32_t TeamSize = GetTeamSize();
    if (HasSquadsInTeams())
    {
        return std::clamp(std::min(Settings.Count, Room), 0, 100);
    }
    if (Room < Settings.Count && TeamSize > 1)
    {
        Room = Room / TeamSize * TeamSize;
    }
    if (PlaylistTeams > 0)
    {
        Room = std::min(Room, (PlaylistTeams - static_cast<int32_t>(HumanTeams().size())) * TeamSize);
    }
    return std::clamp(std::min(Settings.Count, Room), 0, 100);
}

uint16_t FCoreAthenaBotManager::SquadKeyOf(const FCoreAthenaBotController* Bot) const
{
    return SquadKey(Bot->GetTeam(), HasSquadsInTeams() ? Bot->GetSquad() : 0);
}

std::map<uint16_t, int32_t> FCoreAthenaBotManager::PlayerTeamSlots() const
{
    // "Fill players' teams with bots", in duos and squads (teams of 2 to 4, not solo), and in a large team game (Teams of
    // 20) a player's squad of 4 (AFortPlayerStateAthena::SquadId, the match assignment's or Core's for a late
    // player; a player without one yet waits for it). A team (squad) gets bots once a player of it is in the match, for
    // the places its players don't take: those in the match and those its reservations still bring (a party's other
    // members, or another party that fills, loading in; a reservation has a team, not a squad, so only duos' and squads'
    // count). A team with a player whose party doesn't fill (the backend's noFill) gets none. The bots count in the
    // match's size: no more of them than its room leaves.
    std::map<uint16_t, int32_t> Slots;
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    const int32_t SquadSize = bSquads ? GetSquadSize() : PlaylistTeamSize;
    if (!bEnabled || !Settings.bFillPlayerTeams || !GameState || SquadSize < 2 || SquadSize > MaxSquadSize)
    {
        return Slots;
    }
    std::map<uint16_t, std::set<std::wstring>> Players;
    int32_t InMatch = 0;
    int32_t Unnamed = 0;
    for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (!AthenaState || FindBot(reinterpret_cast<const UObject*>(AthenaState)) || AthenaState->TeamIndex == EFortTeam::GetSpectator())
        {
            continue;
        }
        ++InMatch;
        const uint8_t Squad = bSquads ? static_cast<uint8_t>(AthenaState->SquadId) : 0;
        if (bSquads && Squad == 0)
        {
            continue;
        }
        const bool bHasId = AthenaState->UniqueId.IsValid();
        Players[SquadKey(static_cast<uint8_t>(AthenaState->TeamIndex), Squad)].insert(bHasId ? AthenaState->UniqueId.ToString().ToStdString() : std::format(L"#{}", Unnamed++));
    }
    for (const auto& [Team, Id] : Athena->GetTeamReservations())
    {
        const auto Found = bSquads ? Players.end() : Players.find(SquadKey(Team, 0));
        if (Found != Players.end() && Found->second.insert(Id).second)
        {
            ++InMatch;
        }
    }
    const int32_t Capacity = MatchCapacity();
    int32_t Room = Capacity > 0 ? Capacity - std::max(InMatch, Athena->GetNumConnectedPlayers()) : 100;
    for (const auto& [Key, Ids] : Players)
    {
        const bool bNoFill = std::any_of(Ids.begin(), Ids.end(), [this](const std::wstring& Id)
        {
            return std::find(Settings.NoFillAccounts.begin(), Settings.NoFillAccounts.end(), Id) != Settings.NoFillAccounts.end();
        });
        const int32_t Free = std::min(SquadSize - static_cast<int32_t>(Ids.size()), Room);
        if (!bNoFill && Free > 0)
        {
            Slots[Key] = Free;
            Room -= Free;
        }
    }
    return Slots;
}

int32_t FCoreAthenaBotManager::CountAliveBots(bool bPlayersTeams) const
{
    return static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [bPlayersTeams](const auto& Bot) { return Bot->IsAlive() && Bot->IsOnPlayersTeam() == bPlayersTeams; }));
}

void FCoreAthenaBotManager::TickWarmup(float Now)
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
    const std::map<uint16_t, int32_t> TeamSlots = PlayerTeamSlots();
    int32_t TeamBots = 0;
    for (const auto& [Key, Free] : TeamSlots)
    {
        TeamBots += Free;
    }
    const int32_t Wanted = WantedBotCount(TeamBots);
    if (Wanted != LastLoggedWanted || TeamBots != LastLoggedTeamBots)
    {
        AFortGameModeAthena* Athena = GetGameMode();
        Log::Info(L"Battle Royale bots: {} bots for the match (asked for {}, room for {} of {} with {} players connected){}", Wanted, Settings.Count,
            Athena ? MatchCapacity() - Athena->GetNumConnectedPlayers() : 0, MatchCapacity(), Athena ? Athena->GetNumConnectedPlayers() : 0,
            Settings.bFillPlayerTeams ? std::format(L", and {} in the empty places of {} players' teams", TeamBots, TeamSlots.size()) : std::wstring());
        LastLoggedWanted = Wanted;
        LastLoggedTeamBots = TeamBots;
    }
    RemoveSurplusBots(Wanted, TeamSlots);
    const int32_t Missing = std::max(Wanted - CountAliveBots(false), 0) + std::max(TeamBots - CountAliveBots(true), 0);
    if (Missing > 0 && Now >= NextPacedSpawn)
    {
        // Bots come in as players do (#233): none before the first player is on the island (a server nobody joined
        // spends nothing on them), then a few at a time, a little more when another player joins, all of them in a
        // while before the bus. -CoreBotMatch (no players) brings them in at once. A player's team's bots come first.
        AFortGameModeAthena* Athena = GetGameMode();
        AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
        const int32_t Humans = Athena ? Athena->GetNumConnectedPlayers() : 0;
        const bool bBotMatch = Configuration::Get().bBotMatch;
        if (bBotMatch || HasHumanSpawned())
        {
            const float EndTime = GameState && AFortGameStateAthena::HasWarmupCountdown() ? static_cast<float>(GameState->WarmupCountdownEndTime) : 0.0f;
            // Everyone in 20 s before the bus (the warmup's countdown), spread evenly over the time left.
            const float TimeLeft = EndTime > 0.0f ? EndTime - 20.0f - Now : 0.0f;
            float Interval = bBotMatch || TimeLeft <= 0.0f ? 0.0f : std::clamp(TimeLeft / static_cast<float>(Missing), 0.0f, 6.0f);
            int32_t Batch = 1;
            if (Humans > LastHumanCount && !bBotMatch)
            {
                // Another player joined: a couple of bots with them.
                Batch = std::min(Missing, 1 + static_cast<int32_t>(FMath::Rand() % 3));
            }
            LastHumanCount = Humans;
            if (!bLoggedSpawnStart)
            {
                bLoggedSpawnStart = true;
                Log::Info(L"Battle Royale bots: {} spawning {} bots, about one every {:.1f} s ({:.0f} s of warmup left)", bBotMatch ? L"-CoreBotMatch:" : L"a player is on the island:",
                    Missing, Interval, EndTime > 0.0f ? EndTime - Now : -1.0f);
            }
            for (int32_t Index = 0; Index < Batch; ++Index)
            {
                SpawnNextBot(Wanted, TeamSlots, Now);
                NextRegisterTime = std::min(NextRegisterTime, Now);
            }
            NextPacedSpawn = Now + (Interval > 0.0f ? Interval * RandomRange(0.6f, 1.4f) : 0.0f);
        }
    }
    // The navigation (its octree and first build take a quarter of a second in one frame) and the assets the bots load
    // on first use (emotes) are set up as soon as the first bots are in, not as the bus leaves with every player on
    // the server ("ping spikes" with bots on, #233).
    if (!bNavigationTried && Wanted + TeamBots > 0 && GetAliveBotCount() > 0)
    {
        bNavigationTried = true;
        FCoreAthenaBotController::PreloadAssets();
        RefreshLoot(Now);
        GatherContainers();
        InitializeNavigation();
    }
#endif
}

bool FCoreAthenaBotManager::HasHumanSpawned() const
{
    // A player on the island: a player state that isn't a bot's, whose controller has a pawn.
    AFortGameStateAthena* GameState = AthenaGameStateOf(GetGameMode());
    if (!GameState)
    {
        return false;
    }
    for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        AController* Owner = AthenaState && !FindBot(reinterpret_cast<const UObject*>(AthenaState)) ? Cast<AController>(reinterpret_cast<UObject*>(AthenaState->GetOwner())) : nullptr;
        if (Owner && static_cast<APawn*>(Owner->Pawn))
        {
            return true;
        }
    }
    return false;
}

bool FCoreAthenaBotManager::SpawnNextBot(int32_t Wanted, const std::map<uint16_t, int32_t>& TeamSlots, float Now)
{
    if (Now < NextRegisterTime)
    {
        return false;
    }
    // A player's team (squad) first: its bots are in with it, well before the bus.
    for (const auto& [Key, Free] : TeamSlots)
    {
        const int32_t OnTeam = static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [this, Key](const auto& Bot)
        {
            return Bot->IsAlive() && Bot->IsOnPlayersTeam() && SquadKeyOf(Bot.get()) == Key;
        }));
        if (OnTeam < Free)
        {
            if (!RegisterBot(Now, static_cast<uint8_t>(Key & 0xFF), static_cast<uint8_t>(Key >> 8), true))
            {
                // Try again in a while (a spawn failed).
                NextRegisterTime = Now + 2.0f;
                return false;
            }
            return true;
        }
    }
    if (CountAliveBots(false) >= Wanted)
    {
        return false;
    }
    uint8_t Team = 0xFF;
    uint8_t Squad = 0;
    if (!GetNextBotTeamAndSquadId(Team, Squad))
    {
        if (!bLoggedCap)
        {
            bLoggedCap = true;
            Log::Warning(L"Battle Royale bots: no team is free for another bot");
        }
        NextRegisterTime = Now + 2.0f;
        return false;
    }
    if (!RegisterBot(Now, Team, Squad, false))
    {
        NextRegisterTime = Now + 2.0f;
        return false;
    }
    return true;
}

void FCoreAthenaBotManager::RemoveSurplusBots(int32_t Wanted, const std::map<uint16_t, int32_t>& TeamSlots)
{
    // Bots on players' teams (squads): a team keeps as many as it has empty places (a party member loading in late takes
    // one back; the setting turned off, or a player without fill joining the team, takes them all). Bots on teams of their
    // own: the newest go first, a bot on a team a player has since joined (late joining) before them; in a large team
    // game, whose teams players and bots share, only the newest.
    std::map<uint16_t, int32_t> OnTeams;
    for (const auto& Bot : PlayerBots)
    {
        OnTeams[SquadKeyOf(Bot.get())] += Bot->IsAlive() && Bot->IsOnPlayersTeam() ? 1 : 0;
    }
    bool bSurplus = CountAliveBots(false) > Wanted;
    for (const auto& [Key, OnTeam] : OnTeams)
    {
        const auto Slot = TeamSlots.find(Key);
        bSurplus = bSurplus || OnTeam > (Slot != TeamSlots.end() ? Slot->second : 0);
    }
    if (!bSurplus)
    {
        return;
    }
    const std::set<uint8_t> Humans = HasSquadsInTeams() ? std::set<uint8_t>() : HumanTeams();
    std::stable_sort(PlayerBots.begin(), PlayerBots.end(), [&Humans](const auto& A, const auto& B)
    {
        const bool bAHuman = !A->IsOnPlayersTeam() && Humans.contains(A->GetTeam());
        const bool bBHuman = !B->IsOnPlayersTeam() && Humans.contains(B->GetTeam());
        if (bAHuman != bBHuman)
        {
            return !bAHuman;
        }
        return A->GetBotIndex() < B->GetBotIndex();
    });
    std::map<uint16_t, int32_t> Kept;
    int32_t KeptOwn = 0;
    for (auto It = PlayerBots.begin(); It != PlayerBots.end();)
    {
        FCoreAthenaBotController* Bot = It->get();
        bool bKeep = true;
        int32_t Room = Wanted;
        if (Bot->IsAlive() && Bot->IsOnPlayersTeam())
        {
            const uint16_t Key = SquadKeyOf(Bot);
            const auto Slot = TeamSlots.find(Key);
            Room = Slot != TeamSlots.end() ? Slot->second : 0;
            bKeep = Kept[Key] < Room;
            Kept[Key] += bKeep ? 1 : 0;
        }
        else if (Bot->IsAlive())
        {
            bKeep = KeptOwn < Wanted;
            KeptOwn += bKeep ? 1 : 0;
        }
        if (bKeep)
        {
            ++It;
            continue;
        }
        Log::Info(L"Battle Royale bots: {} leaves to make room ({})", Bot->GetBotName(), Bot->IsOnPlayersTeam()
            ? std::format(L"{} has {} empty places for bots", EFortTeam{ Bot->GetTeam() }.ToString(), Room)
            : std::format(L"the match wants {} bots on teams of their own", Wanted));
        Bot->Destroy();
        It = PlayerBots.erase(It);
    }
}

bool FCoreAthenaBotManager::GetNextBotTeamAndSquadId(uint8_t& OutTeam, uint8_t& OutSquadId) const
{
    // 21.40's GetNextBotTeamAndSquadId (8.30's RegisterBot fills the playlist's teams in order, cycling LastBotTeam).
    // Core's bots play on teams of their own (the owner's choice): a team no player is on, from the last (HumanPvP_Team100
    // on solo's 100 teams) down, while players' teams come from the first, filled to the playlist's team size.
    // A large team game (Teams of 20's 5 teams of 20) has no teams of their own to give them: they fill
    // the playlist's teams in squads of 4, as its players do. The team with the fewest players (those in the match or
    // reserved, and its bots) that has room, the first on a tie; in it, a bots' squad with room, else a new squad.
    OutTeam = 0xFF;
    OutSquadId = 0;
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    const int32_t TeamSize = GetTeamSize();
    const int32_t NumTeams = PlaylistTeams > 0 ? std::min(PlaylistTeams, 100) : 100;
    const uint8_t FirstTeam = EFortTeam::GetHumanPvP_Team1();
    if (!HasSquadsInTeams())
    {
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
                OutTeam = Team;
                return true;
            }
        }
        return false;
    }
    if (!Athena || !GameState || PlaylistTeams <= 0)
    {
        return false;
    }

    std::map<uint8_t, std::set<std::wstring>> Players;
    for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (AthenaState && !FindBot(reinterpret_cast<const UObject*>(AthenaState)) && AthenaState->TeamIndex != EFortTeam::GetSpectator())
        {
            Players[static_cast<uint8_t>(AthenaState->TeamIndex)].insert(AthenaState->UniqueId.IsValid() ? AthenaState->UniqueId.ToString().ToStdString() : AthenaState->GetName().ToStdString());
        }
    }
    for (const auto& [Team, Id] : Athena->GetTeamReservations())
    {
        Players[Team].insert(Id);
    }
    int32_t Fewest = TeamSize;
    for (int32_t Index = 0; Index < NumTeams; ++Index)
    {
        const uint8_t Team = static_cast<uint8_t>(FirstTeam + Index);
        const auto Found = Players.find(Team);
        const int32_t OnTeam = (Found != Players.end() ? static_cast<int32_t>(Found->second.size()) : 0)
            + static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [Team](const auto& Bot) { return Bot->IsAlive() && Bot->GetTeam() == Team; }));
        if (OnTeam < Fewest)
        {
            Fewest = OnTeam;
            OutTeam = Team;
        }
    }
    if (OutTeam == 0xFF)
    {
        return false;
    }
    // The fullest of the team's bots' squads that has room, so they make whole squads.
    std::map<uint8_t, int32_t> BotSquads;
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive() && !Bot->IsOnPlayersTeam() && Bot->GetTeam() == OutTeam && Bot->GetSquad() != 0)
        {
            ++BotSquads[Bot->GetSquad()];
        }
    }
    int32_t Fullest = 0;
    for (const auto& [Squad, Members] : BotSquads)
    {
        if (Members < GetSquadSize() && Members > Fullest)
        {
            Fullest = Members;
            OutSquadId = Squad;
        }
    }
    if (OutSquadId == 0)
    {
        OutSquadId = GetNextUniqueSquadID();
    }
    return true;
}

uint8_t FCoreAthenaBotManager::GetNextUniqueSquadID() const
{
    // A squad no player state (a player's or a bot's) is in. The match assignment's squads count up from 1 and its players
    // may still be loading, so, as late players' do (PlaceLatePlayerInSquad), the bots' count down from 255, clear of them.
    std::set<uint8_t> Used;
    AFortGameStateAthena* GameState = AthenaGameStateOf(GetGameMode());
    if (GameState && AFortPlayerStateAthena::HasSquadId())
    {
        for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            if (AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState)))
            {
                Used.insert(static_cast<uint8_t>(AthenaState->SquadId));
            }
        }
    }
    for (const auto& Bot : PlayerBots)
    {
        Used.insert(Bot->GetSquad());
    }
    for (uint32_t Candidate = 255; Candidate > 0; --Candidate)
    {
        if (!Used.contains(static_cast<uint8_t>(Candidate)))
        {
            return static_cast<uint8_t>(Candidate);
        }
    }
    return 0;
}

void FCoreAthenaBotManager::PlaceBotOnTeam(FCoreAthenaBotController* Bot)
{
#if WITH_SERVER_CODE
    // 21.40's AFortGameModeAthena::PlaceBotOnTeam: the bot's controller joins its team as a joining player's does in
    // PlacePlayerOnTeam, with the game mode's AttemptChangeTeamInfoForController: the team's AFortTeamInfo takes the
    // controller (TeamMembers) and its player state the team (PlayerTeam and PlayerTeamPrivate, replicated, and
    // TeamIndex). Then its squad (SquadId: the squad bar's, and the squad's relevancy), as Battle Royale's
    // PlacePlayerOnTeam (3.2 0x9C6240) gives a player theirs. Without the finder's slot, or for a team without its
    // AFortTeamInfo, the bot keeps the old way: TeamIndex alone.
    AFortGameModeAthena* Athena = GetGameMode();
    AAIController* Controller = Bot->GetController();
    AFortPlayerStateAthena* State = Bot->GetPlayerState();
    if (!Athena || !Controller || !State)
    {
        return;
    }
    const uint8_t Team = Bot->GetTeam();
    if (Finder::ResolveTeamChangeLive())
    {
        // The team in a whole register: the game compares all of edx with Spectator (3.2 0xB526C0, "cmp r14d, 67h").
        CallVirtual<bool>(static_cast<AFortGameMode*>(Athena), Natives::AFortGameMode_AttemptChangeTeamInfoForControllerIndex, static_cast<uint32_t>(Team),
            reinterpret_cast<AController*>(Controller));
    }
    static bool bLoggedTeam = false;
    if (static_cast<uint8_t>(State->TeamIndex) != Team)
    {
        State->TeamIndex = EFortTeam{ Team };
        if (!bLoggedTeam)
        {
            bLoggedTeam = true;
            Log::Warning(L"Battle Royale bots: {} isn't in {}'s AFortTeamInfo, so only its TeamIndex is set", Bot->GetBotName(), EFortTeam{ Team }.ToString());
        }
    }
    else if (!bLoggedTeam)
    {
        bLoggedTeam = true;
        AFortTeamInfo* TeamInfo = static_cast<AFortTeamInfo*>(State->PlayerTeam);
        Log::Info(L"Battle Royale bots: {} joins {} as a player does ({})", Bot->GetBotName(), EFortTeam{ Team }.ToString(),
            TeamInfo ? TeamInfo->GetName().ToStdString() : std::wstring(L"no AFortTeamInfo"));
    }
    if (Bot->GetSquad() != 0 && AFortPlayerStateAthena::HasSquadId())
    {
        State->SquadId = Bot->GetSquad();
    }
    State->ForceNetUpdate();
#endif
}

bool FCoreAthenaBotManager::RegisterBot(float Now, uint8_t Team, uint8_t Squad, bool bPlayersTeam)
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    UWorld* World = Athena ? Athena->GetWorld() : nullptr;
    if (!World || Team == 0xFF)
    {
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
    auto Bot = std::make_unique<FCoreAthenaBotController>(this, Controller, BotIndex, Team);
    Bot->SetSquad(Squad);
    Bot->SetOnPlayersTeam(bPlayersTeam);
    if (NameOrder.empty())
    {
        for (int32_t Index = 0; Index < NumBotNames; ++Index)
        {
            NameOrder.push_back(Index);
        }
        for (int32_t Index = NumBotNames - 1; Index > 0; --Index)
        {
            std::swap(NameOrder[static_cast<size_t>(Index)], NameOrder[static_cast<size_t>(FMath::Rand()) % static_cast<size_t>(Index + 1)]);
        }
    }
    const wchar_t* const BaseName = BotNames[NameOrder[static_cast<size_t>(BotIndex % NumBotNames)]];
    const std::wstring Name = BotIndex < NumBotNames ? std::wstring(BaseName) : std::format(L"{}{}", BaseName, BotIndex / NumBotNames + 1);
    Bot->AssignDisplayName(Name);
    PlaceBotOnTeam(Bot.get());
    Bot->InitializeSkills(Settings.Level);
    FVector Spot = WarmupSpots[static_cast<size_t>(FMath::Rand()) % WarmupSpots.size()];
    // A bot on a player's team (squad) comes in by its player, when they have a pawn on the island.
    if (AFortGameStateAthena* GameState = bPlayersTeam ? AthenaGameStateOf(Athena) : nullptr)
    {
        const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
        for (APlayerState* Other : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            AFortPlayerStateAthena* OtherState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(Other));
            AController* Owner = OtherState && static_cast<uint8_t>(OtherState->TeamIndex) == Team && (!bSquads || static_cast<uint8_t>(OtherState->SquadId) == Squad)
                && !FindBot(reinterpret_cast<const UObject*>(OtherState)) ? Cast<AController>(reinterpret_cast<UObject*>(OtherState->GetOwner())) : nullptr;
            if (APawn* OtherPawn = Owner ? static_cast<APawn*>(Owner->Pawn) : nullptr)
            {
                const FVector Near = LocationOf(reinterpret_cast<const AActor*>(OtherPawn));
                Spot = FVector(Near.X + RandomRange(-250.0f, 250.0f), Near.Y + RandomRange(-250.0f, 250.0f), Near.Z);
                break;
            }
        }
    }
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

void FCoreAthenaBotManager::AddToAliveBots(FCoreAthenaBotController* Bot)
{
    AFortPlayerStateAthena* State = Bot->GetPlayerState();
    Log::Info(L"Battle Royale bots: {} joins {}{} ({}, {})", Bot->GetBotName(), Bot->IsOnPlayersTeam() ? L"a player's team, " : L"on ",
        State ? static_cast<EFortTeam>(State->TeamIndex).ToString() : std::wstring(L"no team"), Bot->GetController() ? Bot->GetController()->GetName().ToStdString() : std::wstring(), Bot->GetPawn() ? Bot->GetPawn()->GetName().ToStdString() : std::wstring(L"no pawn"));
}

void FCoreAthenaBotManager::OnAircraftPhaseStarted(float Now)
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
            // A bot on a player's team waits for its player (UpdatePlayerTeamBots), else goes at the drop zone's end.
            const float T = Bot->IsOnPlayersTeam() && HasAlivePlayerOnTeam(Athena, Bot->GetTeam(), HasSquadsInTeams() ? Bot->GetSquad() : 0) ? 1.0f : std::pow(FMath::FRand(), 1.4f);
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
        if (!bNavigationTried)
        {
            bNavigationTried = true;
            InitializeNavigation();
        }
    }
#endif
}

void FCoreAthenaBotManager::GatherLandingSpots()
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_FreeFalling lands at a POI (_SelectNextPOI). 3.x's named locations are its
    // AFortPoiVolumes tagged Athena.Location.POI.* (Core gives the shapeless ones a box, FortPoiVolume.cpp); the loot
    // elsewhere is in clusters of chests (houses and farms), which count as spots too.
    LandingSpots.clear();
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
    for (const FCoreAthenaBotContainer& Container : Containers)
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
            const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Index];
            bNearNamed = bNearNamed || (Spot.Location.X - Center.X) * (Spot.Location.X - Center.X) + (Spot.Location.Y - Center.Y) * (Spot.Location.Y - Center.Y) < Spot.Radius * Spot.Radius;
        }
        if (!bNearNamed)
        {
            LandingSpots.push_back({ Center, 4500.0f, std::format(L"chests at ({:.0f}, {:.0f})", Center.X, Center.Y), LootAround(Center, 4500.0f), 0 });
        }
    }
    std::sort(LandingSpots.begin(), LandingSpots.end(), [](const FCoreAthenaBotLandingSpot& A, const FCoreAthenaBotLandingSpot& B) { return A.Loot > B.Loot; });
    std::wstring Top;
    for (size_t Index = 0; Index < LandingSpots.size() && Index < 8; ++Index)
    {
        Top += std::format(L"{}{} ({:.0f})", Index ? L", " : L"", LandingSpots[Index].Name, LandingSpots[Index].Loot);
    }
    Log::Info(L"Battle Royale bots: {} landing spots, {} of them named locations; the richest: {}", LandingSpots.size(), Named, Top);
#endif
}

float FCoreAthenaBotManager::LootAround(const FVector& Center, float Radius) const
{
    float Value = 0.0f;
    for (const FCoreAthenaBotContainer& Container : Containers)
    {
        const float Dx = Container.Location.X - Center.X;
        const float Dy = Container.Location.Y - Center.Y;
        Value += Dx * Dx + Dy * Dy < Radius * Radius ? (Container.bChest ? 1.0f : 0.3f) : 0.0f;
    }
    for (const FCoreAthenaBotLoot& Entry : Loot)
    {
        const float Dx = Entry.Location.X - Center.X;
        const float Dy = Entry.Location.Y - Center.Y;
        Value += Entry.bWeapon && Dx * Dx + Dy * Dy < Radius * Radius ? 0.3f : 0.0f;
    }
    return Value;
}

int32_t FCoreAthenaBotManager::FindNextBestPOI(const FVector& From, const std::vector<int32_t>& Visited) const
{
#if WITH_SERVER_CODE
    // 21.40's UFortAthenaAIBotEvaluator_SelectNextPOI::FindNextBestPOI: the spots are the world's own (its AFortPoiVolumes'
    // Athena.Location.POI.* tags and its chest clusters, GatherLandingSpots), so every build's map has its own. A spot
    // counts with loot still in it (its containers unsearched), inside the next safe zone, and near (Core's 🟡: within
    // 400 m); the loot left weighs it, the distance (squared) and the bots already there count against it.
    constexpr float MaxDistance = 40000.0f;
    FVector ZoneCenter;
    float ZoneRadius = 0.0f;
    const bool bHasZone = GetSafeZone(ZoneCenter, ZoneRadius);
    int32_t Best = -1;
    float BestScore = 0.0f;
    for (size_t Index = 0; Index < LandingSpots.size(); ++Index)
    {
        const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Index];
        if (std::find(Visited.begin(), Visited.end(), static_cast<int32_t>(Index)) != Visited.end())
        {
            continue;
        }
        const float Dx = Spot.Location.X - From.X;
        const float Dy = Spot.Location.Y - From.Y;
        const float Distance = std::sqrt(Dx * Dx + Dy * Dy);
        if (Distance > MaxDistance || Distance < Spot.Radius * 0.5f)
        {
            continue;
        }
        if (bHasZone)
        {
            const float Zx = Spot.Location.X - ZoneCenter.X;
            const float Zy = Spot.Location.Y - ZoneCenter.Y;
            if (std::sqrt(Zx * Zx + Zy * Zy) > ZoneRadius * 0.85f)
            {
                continue;
            }
        }
        const float Value = LootAround(Spot.Location, Spot.Radius);
        if (Value < 1.0f)
        {
            continue;
        }
        int32_t BotsThere = 0;
        for (const auto& Bot : PlayerBots)
        {
            if (Bot->GetState() != ECoreAthenaBotState::Landed || !Bot->GetPawn())
            {
                continue;
            }
            const FVector Other = LocationOf(reinterpret_cast<const AActor*>(Bot->GetPawn()));
            const float Ox = Other.X - Spot.Location.X;
            const float Oy = Other.Y - Spot.Location.Y;
            BotsThere += Bot->GetNextPOI() == static_cast<int32_t>(Index) || Ox * Ox + Oy * Oy < Spot.Radius * Spot.Radius ? 1 : 0;
        }
        // The nearest come first (a big named location 300 m off outscored the chests next door): the weight falls with
        // the square of the distance in 100 m.
        const float Hundreds = Distance / 10000.0f;
        const float Score = Value * RandomRange(0.8f, 1.2f) / (1.0f + Hundreds * Hundreds) / (1.0f + 0.5f * static_cast<float>(BotsThere));
        if (Score > BestScore)
        {
            BestScore = Score;
            Best = static_cast<int32_t>(Index);
        }
    }
    return Best;
#else
    return -1;
#endif
}

int32_t FCoreAthenaBotManager::FindPOIAt(const FVector& Location) const
{
    int32_t Found = -1;
    float Nearest = 1e18f;
    for (size_t Index = 0; Index < LandingSpots.size(); ++Index)
    {
        const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Index];
        const float DistanceSquared = (Spot.Location.X - Location.X) * (Spot.Location.X - Location.X) + (Spot.Location.Y - Location.Y) * (Spot.Location.Y - Location.Y);
        if (DistanceSquared < Spot.Radius * Spot.Radius && DistanceSquared < Nearest)
        {
            Nearest = DistanceSquared;
            Found = static_cast<int32_t>(Index);
        }
    }
    return Found;
}

const FCoreAthenaBotLandingSpot* FCoreAthenaBotManager::GetPOI(int32_t Index) const
{
    return Index >= 0 && Index < static_cast<int32_t>(LandingSpots.size()) ? &LandingSpots[static_cast<size_t>(Index)] : nullptr;
}

int32_t FCoreAthenaBotManager::CountHuntersOf(const AFortPawn* Target, const FCoreAthenaBotController* Except) const
{
    return static_cast<int32_t>(std::count_if(PlayerBots.begin(), PlayerBots.end(), [Target, Except](const auto& Bot) { return Bot.get() != Except && Bot->IsAlive() && Bot->GetHuntTarget() == Target; }));
}

void FCoreAthenaBotManager::UpdateFlightPath(float Now)
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

void FCoreAthenaBotManager::AssignLandingSpots(float Now)
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
        if (!Bot->IsAlive() || Bot->GetState() != ECoreAthenaBotState::InAircraft)
        {
            continue;
        }
        // A bot on a player's team goes when its player does (UpdatePlayerTeamBots: FollowTeammateDrop): its spot and
        // jump are for a player who leaves it on the bus, at the drop zone's end.
        if (Bot->IsFollowingTeammateDrop())
        {
            ++Assigned;
            continue;
        }
        const bool bWaitsForPlayer = Bot->IsOnPlayersTeam() && HasAlivePlayerOnTeam(GetGameMode(), Bot->GetTeam());
        if (const auto Chosen = TeamSpots.find(Bot->GetTeam()); Chosen != TeamSpots.end())
        {
            const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Chosen->second.first->Spot];
            Bot->SetLandingSpot(Spot.Location, Spot.Radius, bWaitsForPlayer ? Latest : std::clamp(Chosen->second.second + RandomRange(0.1f, 0.6f), Earliest, Latest));
            ++Assigned;
            continue;
        }
        const FCandidate* Best = nullptr;
        float BestScore = -1.0f;
        for (const FCandidate& Candidate : Candidates)
        {
            const FCoreAthenaBotLandingSpot& Spot = LandingSpots[Candidate.Spot];
            const float Shared = (1.0f + static_cast<float>(Spot.Bots)) * (1.0f + static_cast<float>(Spot.Bots));
            const float Score = bHotDrop ? Spot.Loot : Spot.Loot * RandomRange(0.7f, 1.3f) * (1.0f - 0.4f * Candidate.Reach / MaxReach) / Shared;
            if (Score > BestScore)
            {
                BestScore = Score;
                Best = &Candidate;
            }
        }
        FCoreAthenaBotLandingSpot& Spot = LandingSpots[Best->Spot];
        ++Spot.Bots;
        const float JumpAt = std::clamp(Best->Time + RandomRange(-1.5f, 1.5f), Earliest, Latest);
        Bot->SetLandingSpot(Spot.Location, Spot.Radius, bWaitsForPlayer ? Latest : JumpAt);
        if (PlaylistTeamSize > 1)
        {
            TeamSpots[Bot->GetTeam()] = { Best, JumpAt };
        }
        ++Assigned;
    }
    std::wstring Used;
    int32_t SpotsUsed = 0;
    for (const FCoreAthenaBotLandingSpot& Spot : LandingSpots)
    {
        if (Spot.Bots > 0)
        {
            Used += std::format(L"{}{} x{}", SpotsUsed++ ? L", " : L"", Spot.Name, Spot.Bots);
        }
    }
    Log::Info(L"Battle Royale bots: {} bots land at {} of {} reachable spots: {}", Assigned, SpotsUsed, Candidates.size(), Used);
#endif
}

void FCoreAthenaBotManager::GatherContainers()
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

void FCoreAthenaBotManager::OnContainerSearched(ABuildingContainer* Container)
{
    std::erase_if(Containers, [Container](const FCoreAthenaBotContainer& Entry) { return Entry.Container.Get() == Container; });
    // Its loot is on the floor now: the bots see it on the next tick.
    NextLootRefresh = 0.0f;
}

void FCoreAthenaBotManager::InitializeNavigation()
{
#if WITH_SERVER_CODE
    // The island: everything the bots go to (the loot and the containers), with room around it.
    FBox Island;
    for (const FCoreAthenaBotLoot& Entry : Loot)
    {
        Island += Entry.Location;
    }
    for (const FCoreAthenaBotContainer& Entry : Containers)
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

void FCoreAthenaBotManager::UpdateCounts()
{
    // 21.40 counts bots in the game state's players and teams left (AddToAliveBots / RemoveFromAliveBots); 3.0's game
    // mode only counts its alive players, so the bots are added to its numbers.
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (!GameState)
    {
        return;
    }
    // Each team's count too, until the last bot that was in it is gone (the warmup's surplus bots leave).
    UpdateTeamPlayersLeft(Athena, GameState);
    if ((bAircraftStarted && BotsAtStart == 0) || (!bAircraftStarted && PlayerBots.empty()))
    {
        return;
    }
    // On the warmup island too: its player count (the game state's, which the game mode counts for its players) has
    // the bots in it, as 21.40's AddToAliveBots counts them.
    const int32_t PlayersLeft = Athena->AlivePlayers.Num() + GetAliveBotCount();
    if (!bAircraftStarted && AFortGameStateAthena::HasTotalPlayers() && GameState->TotalPlayers != PlayersLeft)
    {
        GameState->TotalPlayers = PlayersLeft;
    }
    const int32_t TeamsLeft = static_cast<int32_t>(AliveTeams(true).size());
    if (GameState->PlayersLeft != PlayersLeft || GameState->TeamsLeft != TeamsLeft)
    {
        GameState->PlayersLeft = PlayersLeft;
        GameState->TeamsLeft = TeamsLeft;
        GameState->ForceNetUpdate();
    }
}

void FCoreAthenaBotManager::UpdateTeamPlayersLeft(AFortGameModeAthena* Athena, AFortGameStateAthena* GameState)
{
    // The HUD's "1 : 0" in Teams of 20 with bots on every team (#242): AddToAlivePlayers (3.2 0x9AE8D0) counts each
    // player into the game mode's TeamAlivePlayers and, in a large team game, the game state's TeamXPlayersLeft (growing
    // it to the player's team), and RemoveFromAlivePlayers (0x9CA730) takes them out of both. The bots' controllers aren't
    // players' (AddToAlivePlayers writes AFortPlayerControllerAthena's members), so, as 21.40's AddToAliveBots and
    // RemoveFromAliveBots do, each team's count is its alive players' and its alive bots': the game's own count of the
    // team's players, and Core's of its bots. TeamAlivePlayers stays the players' (the game's own win check, which
    // ShouldHoldNativeWinCheck holds while bots are in, reads it). The HUD redraws its count when PlayersLeft changes
    // (3.2 0x2746410), which UpdateCounts sets after this; the map screen reads it each frame (0x2747150).
    if (!AFortGameStateAthena::HasTeamXPlayersLeft() || !AFortGameModeAthena::HasTeamAlivePlayers() || !HasSquadsInTeams())
    {
        return;
    }
    TArray<int32_t>& TeamXPlayersLeft = GameState->GetMember_TeamXPlayersLeft();
    // Not a large team game's (the playlist didn't size it, and no player was counted into it).
    if (TeamXPlayersLeft.Num() == 0)
    {
        return;
    }
    const uint8_t FirstTeam = EFortTeam::GetHumanPvP_Team1();
    std::map<int32_t, int32_t> Bots;
    for (const auto& Bot : PlayerBots)
    {
        if (Bot->IsAlive() && Bot->GetTeam() >= FirstTeam)
        {
            ++Bots[Bot->GetTeam() - FirstTeam];
        }
    }
    const int32_t Teams = std::max(PlaylistTeams, Bots.empty() ? 0 : Bots.rbegin()->first + 1);
    if (TeamXPlayersLeft.Num() < Teams)
    {
        TeamXPlayersLeft.AddZeroed(Teams - TeamXPlayersLeft.Num());
    }
    bool bChanged = false;
    for (int32_t Index = 0; Index < TeamXPlayersLeft.Num(); ++Index)
    {
        const auto Found = Bots.find(Index);
        const int32_t Left = Athena->GetTeamAlivePlayers(Index) + (Found != Bots.end() ? Found->second : 0);
        if (TeamXPlayersLeft[Index] != Left)
        {
            TeamXPlayersLeft[Index] = Left;
            bChanged = true;
        }
    }
    if (bChanged)
    {
        GameState->ForceNetUpdate();
    }
}

void FCoreAthenaBotManager::RemoveFromAliveBots(FCoreAthenaBotController* Bot, AFortPlayerStateAthena* KillerState)
{
#if WITH_SERVER_CODE
    // 21.40's AFortGameModeAthena::RemoveFromAliveBots. The team's place is the teams left when its last player is out
    // (counted before this bot is).
    AFortGameModeAthena* Athena = GetGameMode();
    const int32_t TeamsBefore = static_cast<int32_t>(AliveTeams(true).size());
    AFortPlayerStateAthena* State = Bot->GetPlayerState();
    FCoreAthenaBotController* KillerBot = FindBot(KillerState);
    if (KillerBot && KillerBot != Bot)
    {
        KillerBot->AddKill();
        // A bot on a player's team: the elimination is its team's, as a player teammate's is.
        if (KillerBot->IsOnPlayersTeam())
        {
            AddTeamKillScore(Athena, KillerBot->GetTeam());
        }
        // The tags a player's kill event for this bot would carry, once a match (what the kill challenges match).
        if (!bLoggedKillTags)
        {
            bLoggedKillTags = true;
            FGameplayTagContainer TargetTags;
            FGameplayTagContainer SourceTags;
            AddKillEventTags(reinterpret_cast<UObject*>(Bot->GetPawn()), reinterpret_cast<AController*>(KillerBot->GetController()), TargetTags, SourceTags);
            const auto Names = [](const FGameplayTagContainer& Tags)
            {
                std::wstring Text;
                for (const FGameplayTag& Tag : Tags.GameplayTags)
                {
                    Text += (Text.empty() ? L"" : L", ") + Tag.TagName.ToStdString();
                }
                return Text.empty() ? std::wstring(L"none") : Text;
            };
            Log::Info(L"Battle Royale bots: a kill event for {} has the target tags [{}] and the killer's weapon tags [{}]", Bot->GetBotName(), Names(TargetTags), Names(SourceTags));
        }
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
        AddTeamKillScore(Athena, static_cast<uint8_t>(KillerState->TeamIndex));

        // Quests: the game's own Kill event (if it sent one for this pawn) went through UFortQuestManager already, with
        // the bot's tags added (SendStatEventWithTagsHK); with the setting on, the game's two events are sent for a kill
        // it didn't report, as its AFortGameModeZone::Killed sends them (3.1 0xB5E890: KillContribution, then Kill),
        // with the tags its Kill has: the dead pawn's for the target, the killer's and their weapon's for the source.
        UObject* Target = reinterpret_cast<UObject*>(Bot->GetPawn());
        const bool bNativeEvent = std::any_of(NativeKillTargets.begin(), NativeKillTargets.end(), [Target](const TWeakObjectPtr<UObject>& Weak) { return Weak.Get() == Target; });
        UFortRegisteredPlayerInfo* PlayerInfo = static_cast<UFortRegisteredPlayerInfo*>(KillerPlayer->MyPlayerInfo);
        UFortQuestManager* QuestManager = PlayerInfo ? PlayerInfo->GetAthenaQuestManager() : nullptr;
        if (Settings.bEliminationsCountForQuests && !bNativeEvent && QuestManager)
        {
            FGameplayTagContainer SourceTags;
            FGameplayTagContainer ContextTags;
            QuestManager->GetSourceAndContextTags(SourceTags, ContextTags);
            FGameplayTagContainer TargetTags;
            AddKillEventTags(Target, reinterpret_cast<AController*>(KillerPlayer), TargetTags, SourceTags);
            QuestManager->SendStatEvent(EFortQuestObjectiveStatEvent::GetKillContribution(), Target, TargetTags, SourceTags, ContextTags, 1, false);
            QuestManager->SendStatEvent(EFortQuestObjectiveStatEvent::GetKill(), Target, TargetTags, SourceTags, ContextTags, 1, false);
        }
        Log::Info(L"Battle Royale bots: {} eliminated {} ({} bots now; quests: {})", KillerState->GetPlayerName().ToStdString(), Bot->GetBotName(), BotKillsOf(KillerState),
            !Settings.bEliminationsCountForQuests ? L"off" : bNativeEvent ? L"the game's event" : QuestManager ? L"Core's event" : L"no quest manager");
    }

    // A player's team is out once its players are out too (Core's death handler left its place to its bots then); in a
    // large team game any of the team's players.
    const bool bPlayersTeam = Bot->IsOnPlayersTeam() || (HasSquadsInTeams() && HumanTeams().contains(Bot->GetTeam()));
    const bool bTeamOut = std::none_of(PlayerBots.begin(), PlayerBots.end(), [Bot](const auto& Other) { return Other.get() != Bot && Other->IsAlive() && Other->GetTeam() == Bot->GetTeam(); })
        && !(bPlayersTeam && HasAlivePlayerOnTeam(Athena, Bot->GetTeam()));
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
        if (bPlayersTeam)
        {
            SetPlayersTeamPlace(Bot->GetTeam(), TeamsBefore);
        }
    }
    Log::Info(L"Battle Royale bots: {} is out{} ({} bots left)", Bot->GetBotName(), bTeamOut && bAircraftStarted ? std::format(L", place {}", TeamsBefore) : std::wstring(), GetAliveBotCount());
    // Every player still in the match outlived the bot ("Outlive opponents", AthenaOutlive), as they outlive a player who
    // is out, with the setting on.
    if (Athena && bAircraftStarted && Settings.bEliminationsCountForQuests)
    {
        Athena->SendOutliveStatEvents(reinterpret_cast<AFortPawn*>(Bot->GetPawn()));
    }
    if (Athena)
    {
        UpdateCounts();
        CheckWinConditionLastManStanding();
    }
#endif
}

void FCoreAthenaBotManager::ApplyBotDamage(FCoreAthenaBotController* Attacker, AFortPawn* Victim, float Damage, UFortWeaponItemDefinition* Weapon)
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
    // A pawn that can't be damaged (everyone on the warmup island: AFortGameModeAthena's warmup restart turns
    // bCanBeDamaged off; a player in god mode) takes nothing, as the game's own damage leaves it.
    if (IsInvulnerable(Victim))
    {
        return;
    }

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
    if (FCoreAthenaBotController* VictimBot = FindBot(reinterpret_cast<const UObject*>(Victim)); VictimBot && VictimBot->CanGoDBNO())
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

bool FCoreAthenaBotManager::IsInvulnerable(AFortPawn* Victim)
{
    // The pawn's damage check (3.3 0xE6AB30) gives a hit no damage while bCanBeDamaged is off or bIsInvulnerable (the
    // god cheat's) is on; the unlimitedhealth cheat's bUnlimitedHealth is read by the same damage path (1.9 0x7AE300).
    // Core's own bot damage (a hit the game's damage didn't take, or a team playlist's bot) asks the same (CoreV2 #252:
    // "cheat god" didn't stop the bots' hits).
    if (!Victim || !Victim->bCanBeDamaged)
    {
        return true;
    }
    if (AFortPawn::HasInvulnerable() && Victim->bIsInvulnerable)
    {
        return true;
    }
    APlayerController* Player = Cast<APlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Victim->Controller)));
    UFortCheatManager* Cheats = Player ? Cast<UFortCheatManager>(static_cast<UObject*>(Player->CheatManager)) : nullptr;
    return Cheats && UFortCheatManager::HasUnlimitedHealthFlag() && Cheats->bUnlimitedHealth;
}

bool FCoreAthenaBotManager::UsesGameDamage(const AFortPawn* Victim) const
{
    return Victim && (!IsBotPawn(reinterpret_cast<const UObject*>(Victim)) || GetTeamSize() <= 1);
}

void FCoreAthenaBotManager::NoteBotHit(FCoreAthenaBotController* Attacker, AFortPawn* Victim)
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

std::vector<FCoreAthenaBotController*> FCoreAthenaBotManager::TeammatesOf(const FCoreAthenaBotController* Bot) const
{
    // In a large team game a bot's squad: the rest of its team of 20 isn't who it lands, regroups and revives with.
    std::vector<FCoreAthenaBotController*> Teammates;
    for (const auto& Other : PlayerBots)
    {
        if (Other.get() != Bot && Other->IsAlive() && SquadKeyOf(Other.get()) == SquadKeyOf(Bot))
        {
            Teammates.push_back(Other.get());
        }
    }
    return Teammates;
}

std::vector<AFortPlayerPawnAthena*> FCoreAthenaBotManager::PlayerTeammatesOf(const FCoreAthenaBotController* Bot) const
{
    std::vector<AFortPlayerPawnAthena*> Teammates;
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena || !Bot->IsOnPlayersTeam())
    {
        return Teammates;
    }
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Player));
        AFortPlayerPawnAthena* Pawn = State && static_cast<uint8_t>(State->TeamIndex) == Bot->GetTeam() && (!bSquads || static_cast<uint8_t>(State->SquadId) == Bot->GetSquad())
            ? Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Player->GetPawn())) : nullptr;
        if (Pawn && !Pawn->bIsDying && !Pawn->IsPendingKill())
        {
            Teammates.push_back(Pawn);
        }
    }
    return Teammates;
}

AFortPlayerStateAthena* FCoreAthenaBotManager::FindPlayerToSpectate(const FCoreAthenaBotController* Bot, AFortPlayerStateAthena* KillerState) const
{
    // SpectateOnDeath's order (3.3 0xA31650): in squad play (the game mode's bSquadPlay) a pawn of the player's squad
    // that isn't dead, then one of their team; then the one who eliminated them (PlayerToSpectateOnDeath), while alive;
    // then the first pawn alive. A bot's choice is among the players still in the match (the game mode's AlivePlayers),
    // the last one at random, so the bots out of a match don't all watch the same player.
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena)
    {
        return nullptr;
    }
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    std::vector<AFortPlayerStateAthena*> Players;
    AFortPlayerStateAthena* Teammate = nullptr;
    AFortPlayerStateAthena* Killer = nullptr;
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Player));
        AFortPawn* Pawn = State && !FindBot(reinterpret_cast<const UObject*>(State)) ? Cast<AFortPawn>(reinterpret_cast<UObject*>(Player->GetPawn())) : nullptr;
        if (!Pawn || Pawn->IsDead() || Pawn->IsPendingKill())
        {
            continue;
        }
        Players.push_back(State);
        if (!Teammate && static_cast<uint8_t>(State->TeamIndex) == Bot->GetTeam() && (!bSquads || static_cast<uint8_t>(State->SquadId) == Bot->GetSquad()))
        {
            Teammate = State;
        }
        if (State == KillerState)
        {
            Killer = State;
        }
    }
    if (Teammate || Killer || Players.empty())
    {
        return Teammate ? Teammate : Killer;
    }
    return Players[static_cast<size_t>(FMath::Rand()) % Players.size()];
}

bool FCoreAthenaBotManager::HasStandingTeammate(const FCoreAthenaBotController* Bot) const
{
    // The game's choice between down and out (AFortPlayerPawnAthena's, 3.3 0xA31C20, after AFortPawn's health ran out,
    // 0xE5BAE0): in squad play, another Athena pawn on the pawn's team, the whole team in a large team game and not its
    // squad, neither DBNO nor dead (AFortPawn::IsDead), skydiving or on the ground.
    const auto Standing = [](AFortPawn* Pawn) { return Pawn && !Pawn->bIsDBNO && !Pawn->IsDead(); };
    for (const auto& Other : PlayerBots)
    {
        if (Other.get() != Bot && Other->GetTeam() == Bot->GetTeam() && Standing(reinterpret_cast<AFortPawn*>(Other->GetPawn())))
        {
            return true;
        }
    }
    AFortGameModeAthena* Athena = GetGameMode();
    if (!Athena)
    {
        return false;
    }
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPlayerStateAthena* State = Player ? AthenaStateOf(reinterpret_cast<AController*>(Player)) : nullptr;
        if (State && static_cast<uint8_t>(State->TeamIndex) == Bot->GetTeam() && Standing(Cast<AFortPawn>(reinterpret_cast<UObject*>(Player->GetPawn()))))
        {
            return true;
        }
    }
    return false;
}

bool FCoreAthenaBotManager::HasPlayersTeamBotsLeft(uint8_t Team) const
{
    const bool bSquads = HasSquadsInTeams();
    return std::any_of(PlayerBots.begin(), PlayerBots.end(), [Team, bSquads](const auto& Bot) { return Bot->IsAlive() && (bSquads || Bot->IsOnPlayersTeam()) && Bot->GetTeam() == Team; });
}

void FCoreAthenaBotManager::SetPlayersTeamPlace(uint8_t Team, int32_t Place)
{
#if WITH_SERVER_CODE
    // What Core's death handler does for the team's last player out (AFortPlayerControllerAthena::OnPawnDied: the game's
    // SetTeamPlace on them): here its bots were the last, so on one of its players, any still connected; every player
    // state of the team takes the place too (one who left has no controller).
    AFortGameStateAthena* GameState = AthenaGameStateOf(GetGameMode());
    if (!GameState)
    {
        return;
    }
    AFortPlayerControllerAthena* Player = nullptr;
    for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
    {
        AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
        if (!AthenaState || static_cast<uint8_t>(AthenaState->TeamIndex) != Team || FindBot(reinterpret_cast<const UObject*>(AthenaState)))
        {
            continue;
        }
        AthenaState->Place = Place;
        AthenaState->ForceNetUpdate();
        Player = Player ? Player : Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(AthenaState->GetOwner()));
    }
    if (Player && Natives::AFortPlayerControllerAthena_SetTeamPlace)
    {
        Natives::AFortPlayerControllerAthena_SetTeamPlace(Player, Place);
    }
    Log::Info(L"Battle Royale bots: {} (a player's team) is out with its bots, place {}", EFortTeam{ Team }.ToString(), Place);
#endif
}

void FCoreAthenaBotManager::UpdatePlayerTeamBots(float Now)
{
#if WITH_SERVER_CODE
    AFortGameModeAthena* Athena = GetGameMode();
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    if (!GameState || std::none_of(PlayerBots.begin(), PlayerBots.end(), [](const auto& Bot) { return Bot->IsAlive() && Bot->IsOnPlayersTeam(); }))
    {
        return;
    }
    // The team's squad (AFortPlayerStateAthena::SquadId: the squad bar lists a squad's players, OnRep_SquadId), once one of
    // its players has one, checked every second. In a large team game the bot has its squad from the start (its player's).
    if (AFortPlayerStateAthena::HasSquadId() && !HasSquadsInTeams() && Now >= NextPlayerTeamSync)
    {
        NextPlayerTeamSync = Now + 1.0f;
        std::map<uint8_t, uint8_t> Squads;
        for (APlayerState* PlayerState : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            AFortPlayerStateAthena* AthenaState = Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(PlayerState));
            if (AthenaState && static_cast<uint8_t>(AthenaState->SquadId) != 0 && !FindBot(reinterpret_cast<const UObject*>(AthenaState)))
            {
                Squads.try_emplace(static_cast<uint8_t>(AthenaState->TeamIndex), static_cast<uint8_t>(AthenaState->SquadId));
            }
        }
        for (const auto& Bot : PlayerBots)
        {
            AFortPlayerStateAthena* State = Bot->IsAlive() && Bot->IsOnPlayersTeam() ? Bot->GetPlayerState() : nullptr;
            const auto Squad = State ? Squads.find(Bot->GetTeam()) : Squads.end();
            if (Squad != Squads.end() && static_cast<uint8_t>(State->SquadId) != Squad->second)
            {
                State->SquadId = Squad->second;
                State->ForceNetUpdate();
                Log::Info(L"Battle Royale bots: {} is in squad {} with its player teammates", Bot->GetBotName(), Squad->second);
            }
        }
    }
    // After the bus: a bot on a player's team goes when its player does, and falls towards them. A player in the aircraft
    // has no pawn (StartAircraftPhase takes the warmup's); the one they have once the drop zone opens is their skydive.
    if (!bAircraftStarted || Now < DropStartTime)
    {
        return;
    }
    const bool bSquads = HasSquadsInTeams() && AFortPlayerStateAthena::HasSquadId();
    std::map<uint16_t, AFortPlayerPawnAthena*> Jumped;
    const TArray<AFortPlayerControllerAthena*>& Alive = Athena->AlivePlayers;
    for (int32_t Index = 0; Index < Alive.Num(); ++Index)
    {
        AFortPlayerControllerAthena* Player = Alive[Index];
        AFortPlayerStateAthena* State = AthenaStateOf(reinterpret_cast<AController*>(Player));
        AFortPlayerPawnAthena* Pawn = State ? Cast<AFortPlayerPawnAthena>(reinterpret_cast<UObject*>(Player->GetPawn())) : nullptr;
        if (Pawn && !Pawn->bIsDying)
        {
            Jumped.try_emplace(SquadKey(static_cast<uint8_t>(State->TeamIndex), bSquads ? static_cast<uint8_t>(State->SquadId) : 0), Pawn);
        }
    }
    for (const auto& Bot : PlayerBots)
    {
        const bool bFalling = Bot->GetState() == ECoreAthenaBotState::InAircraft || Bot->GetState() == ECoreAthenaBotState::Skydiving;
        const auto Player = Bot->IsOnPlayersTeam() && bFalling ? Jumped.find(SquadKeyOf(Bot.get())) : Jumped.end();
        if (Player != Jumped.end())
        {
            AFortPlayerPawnAthena* Pawn = Player->second;
            Bot->FollowTeammateDrop(LocationOf(reinterpret_cast<const AActor*>(Pawn)), !Pawn->bIsSkydiving && !Pawn->bIsParachuteOpen, Now);
        }
    }
#endif
}

AFortPlayerStateAthena* FCoreAthenaBotManager::LastBotAttackerOf(const AFortPawn* Pawn) const
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

void FCoreAthenaBotManager::CheckWinConditionLastManStanding()
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
            // Its bots (a player's team filled with bots) win with it.
            for (const auto& Bot : PlayerBots)
            {
                AFortPlayerStateAthena* State = Bot->IsAlive() && Teams.contains(Bot->GetTeam()) ? Bot->GetPlayerState() : nullptr;
                if (State)
                {
                    State->Place = 1;
                    State->ForceNetUpdate();
                }
            }
            Natives::AFortGameModeAthena_StartEndGamePhase(Athena, Winner);
            return;
        }
        bool bPlayersTeamWon = false;
        for (const auto& Bot : PlayerBots)
        {
            if (Bot->IsAlive())
            {
                if (AFortPlayerStateAthena* State = Bot->GetPlayerState())
                {
                    State->Place = 1;
                    State->ForceNetUpdate();
                }
                bPlayersTeamWon = bPlayersTeamWon || Bot->IsOnPlayersTeam() || (HasSquadsInTeams() && HumanTeams().contains(Bot->GetTeam()));
                Log::Info(L"Battle Royale bots: {} wins the match ({})", Bot->GetBotName(), Bot->IsOnPlayersTeam() ? L"for its players' team" : L"a bots' team");
            }
        }
        // The players whose bots won: their team's place is 1.
        if (bPlayersTeamWon && !Teams.empty())
        {
            SetPlayersTeamPlace(*Teams.begin(), 1);
        }
        Natives::AFortGameModeAthena_StartEndGamePhase(Athena, nullptr);
        return;
    }
    if (HumansAtStart > 0 && HumansLeft == 0)
    {
        bMatchEnded = true;
        Log::Info(L"Battle Royale bots: every player is out with {} bots left, so the match ends", GetAliveBotCount());
        // A players' team whose bots are still in shares the place of the teams left.
        for (const uint8_t Team : Teams)
        {
            if (HasPlayersTeamBotsLeft(Team) && (!HasSquadsInTeams() || HumanTeams().contains(Team)))
            {
                SetPlayersTeamPlace(Team, static_cast<int32_t>(Teams.size()));
            }
        }
        Natives::AFortGameModeAthena_StartEndGamePhase(Athena, nullptr);
    }
#endif
}

void FCoreAthenaBotManager::RefreshEnemies()
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

void FCoreAthenaBotManager::RefreshLoot(float Now)
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
        const bool bConsumable = !bWeapon && !bAmmo && Weapon && FCoreAthenaBotController::GetConsumableInfo(Definition) != nullptr;
        // Wood, stone and metal on the floor, which bots take for building (CoreV2 #252).
        const bool bMaterial = !Weapon && FCoreAthenaBotController::MaterialKindOf(Definition) >= 0;
        if (bWeapon || bAmmo || bConsumable || bMaterial)
        {
            Loot.push_back({ TWeakObjectPtr<AFortPickup>(Pickup), LocationOf(Pickup), bWeapon, bConsumable });
        }
    }
#endif
}

void FCoreAthenaBotManager::LogSummary(float Now)
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
        if (bHasNavigation && Bot->GetState() == ECoreAthenaBotState::Landed && Bot->GetPawn() && UAthenaNavSystem::IsOnNavMesh(World, LocationOf(reinterpret_cast<const AActor*>(Bot->GetPawn()))))
        {
            ++OnNavMesh;
        }
    }
    AFortGameStateAthena* GameState = AthenaGameStateOf(Athena);
    Log::Info(L"Battle Royale bots: {} in the warmup, {} in the aircraft, {} skydiving, {} landed ({} on the navmesh), {} out; {} loot pickups seen; players left {}, teams left {}; "
        L"frame {:.1f} ms average, {:.1f} ms max, {} over 50 ms; bots {:.2f} ms a frame, {:.1f} ms max (one bot {:.1f} ms: {}, loot list {:.1f} ms)",
        ByState[0], ByState[1], ByState[2], ByState[3], OnNavMesh, ByState[4], Loot.size(), GameState ? static_cast<int32_t>(GameState->PlayersLeft) : 0, GameState ? static_cast<int32_t>(GameState->TeamsLeft) : 0,
        FrameCount ? FrameMsSum / FrameCount : 0.0, FrameMsMax, FrameSpikes, FrameCount ? BotsMsSum / FrameCount : 0.0, BotsMsMax, BotTickMsMax, SlowestBotTick, LootRefreshMsMax);
    FrameMsSum = 0.0;
    FrameMsMax = 0.0;
    BotsMsSum = 0.0;
    FrameCount = 0;
    FrameSpikes = 0;
    BotsMsMax = 0.0;
    BotTickMsMax = 0.0;
    LootRefreshMsMax = 0.0;
    SlowestBotTick.clear();
}
