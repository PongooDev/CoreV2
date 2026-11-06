#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
#include "CrashLog.h"
#include "Diagnostics.h"
#include "Finder.h"
#include "GameMapsSettings.h"
#include "Hooks.h"
#include "LaunchEngineLoop.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Log.h"
#include "Packed.h"
#include "Engine/GameEngine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Engine/World.h"
#include "UObject/Linker.h"
#include "UObject/Object.h"
#include "FortGameInstance.h"
#include "FortGameMode.h"
#include "FortGameModeZone.h"
#include "Athena/FortAthenaSupplyDrop.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Misc/CommandLine.h"
#include "FortVisibilityComponent.h"
#include "FortWorldManager.h"
#include "GameThread.h"
#include "McpProfileGroup.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "OnlineSubsystemImpl.h"
#include "OnlineVoiceImpl.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionManager.h"
#include "Online/FortGameSession.h"
#include "Online/FortMcpProfileWorld.h"
#include "AI/FortAIGoalManager.h"
#include "Building/BuildingActor.h"
#include "Building/BuildingContainer.h"
#include "Building/BuildingItemCollectorActor.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "Building/BuildingTrap.h"
#include "Building/BuildingTrapDefender.h"
#include "AI/FortBTTask_SetFrustrationDiscouragement.h"
#include "AI/FortQueryContexts.h"
#include "AI/FortQueryGenerators.h"
#include "Missions/FortObjectiveBase.h"
#include "Items/FortSchematicItem.h"
#include "Items/FortQuickBars.h"
#include "Pawns/FortAIPawn.h"
#include "Pawns/FortPlayerPawn.h"
#include "Quests/FortQuestManager.h"
#include "Player/FortCheatManager.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerControllerDeployableBase.h"
#include "Player/FortPlayerControllerOutpost.h"
#include "Player/FortPlayerControllerZone.h"
#include "Player/FortPlayerState.h"
#include "FortGlobals.h"
#include "FortKismetLibrary.h"
#include "Abilities/FortGameplayAbility.h"
#include "Missions/FortMissionLibrary.h"
#include "AbilitySystemComponent.h"

namespace
{
    // A server has no window to show, and Windows 11 doesn't honour the 1ms timer resolution the engine asks for in a
    // process whose windows are hidden: its frame wait (Sleep, UEngine::UpdateTimeAndHandleMaxTickRate) ended on the
    // 15.6ms system tick, so frames came 15.6, 31.2 or 46.9ms apart (#109). Opting out of that keeps its waits to the ms.
    void HonourTimerResolution()
    {
        PROCESS_POWER_THROTTLING_STATE State = {};
        State.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        State.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        State.StateMask = 0;
        if (!SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &State, sizeof(State)))
        {
            Log::Warning(L"Timer resolution: SetProcessInformation failed ({}), frame waits may end on the system tick", GetLastError());
        }
    }
}

static void Start();
static void InstallHooks();

namespace
{
    // Packed builds only. On the other builds the GetSourceIniHierarchyFilenames detour layers DedicatedServerEngine.ini
    // over a server's config while the engine loads it, during PreInit, which on a packed build is over by the time Core
    // may patch code, and the saved Engine.ini isn't read back into these shipping builds' config (a key written there
    // reads as the build ships it), so the keys a server needs are merged into the loaded config instead, through the
    // engine's own FConfigFile::CombineFromBuffer: DedicatedServerEngine.ini's bHasVoiceEnabled (the server forwards its
    // players' voice) and bEnableDefaultServiceTracker, and the launcher's [XMPP] bEnabled=False (Core-Launcher
    // ServerConfig). DedicatedServerEngine.ini's map and game mode are in DefaultEngine.ini too, and its [Core.Log] is
    // only log verbosity.
    void CombineServerConfig()
    {
        FConfigCacheIni::CombineIntoEngineIni(L"Server config",
            L"[OnlineSubsystem]\r\nbHasVoiceEnabled=true\r\n"
            L"[OnlineSubsystemMcp]\r\nbEnableDefaultServiceTracker=false\r\n"
            L"[XMPP]\r\nbEnabled=false\r\n");
    }
}

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Server");
    Log::Info(L"Core V2 starting");

    // On a packed build the image is still ciphertext at this point and the stub would overwrite anything patched
    // into it, so hand the rest over to a watcher that runs once the stub has finished.
    if (Packed::RunAfterStub(&Start))
    {
        return;
    }

    Start();
}

static void Start()
{
    CrashLog::Install();
    FCommandLine::ApproveServerSwitches();
    HonourTimerResolution();

    // Without its hooks the process would carry on as a game client (a window and a front end, Core-Launcher #4), which
    // the launcher would keep counting as a server: a server that can't start stops.
    if (MH_Initialize() != MH_OK)
    {
        Log::Fatal(L"MinHook failed to initialize");
    }

    // Packed builds only: vtable slots, not code, so they can go in before the packer's check, and first, before the
    // longer finders: the online subsystem starts during PreInit, a second or two after the stub hands over, and makes
    // its voice interface there, as a dedicated server's only when IsDedicated already says so and the config has voice
    // on. The config part goes in on the engine's thread as the subsystem first asks whether it is dedicated. (The
    // config finder reads FOnlineVoiceImpl::Init, so before its slot is swapped.)
    if (Packed::IsPacked())
    {
        if (Finder::ResolveConfigCombine())
        {
            FOnlineSubsystemImpl::BeforeFirstDedicatedCheck = &CombineServerConfig;
        }
        FOnlineVoiceImpl::Hook();
        if (Finder::ResolveOnlineSubsystemIsDedicated())
        {
            FOnlineSubsystemImpl::HookVTables();
        }
    }

    if (!Finder::ResolveRuntime() || !Finder::ResolveServer())
    {
        Log::Fatal(L"Startup aborted: required addresses are missing");
    }

    // Packed builds only: the packer checks its decrypted code once as the engine's initial load ends, and a patch in
    // place by then hangs the game, so the engine starts as the client it is, untouched. Its own thread stops as it
    // enters UEngine::Init, the step after that check, and Core makes it a server there: everything from UEngine::Init
    // on (its game instance, the default map, Listen) then runs with Core's hooks in place, as on the other builds.
    if (Packed::IsPacked())
    {
        if (!Finder::ResolveEngineInit() || !Packed::RunBeforeEngineInit(L"Default__GameEngine", &InstallHooks))
        {
            Log::Error(L"Packed exe: making the engine a server once it is up instead, which may be too late for UEngine::Init");
            Packed::WaitForCodeCheck();
            InstallHooks();
        }
        return;
    }

    InstallHooks();
}

static void InstallHooks()
{
    // On a packed build this runs once PreInit is over. FApp::ShouldUseThreadingForPerformance only matters to PreInit,
    // and its GIsServer/GIsClient stores have run, so HookAfterPreInit sets the globals themselves.
    const bool bAfterPreInit = Packed::IsPacked();
    // 3.5+ packed builds check the code again on the engine's first ticks, and a patch in place by then crashes them at
    // frame 0. Everything below that changes data (GIsClient, the merged config, the MCP permissions, vtable slots)
    // still goes in now, before UEngine::Init; the detours and patched calls and branches are held, and so is the engine,
    // at UEngine::Start: it ticks with no map on unpatched code, and is then patched, made a server (GIsServer) and
    // started. The default map, Listen and the matchmaking registration all come after that, with every hook in place.
    const bool bHoldCode = Packed::ChecksCodeOnFirstTicks();
    if (bHoldCode)
    {
        Hooks::HoldCode();
    }
    if (!bAfterPreInit)
    {
        FApp::Hook();
    }
    FConfigCacheIni::Hook();
    UObject::Hook();
    FLinker::Hook();
    if (bAfterPreInit)
    {
        if (bHoldCode)
        {
            FEngineLoop::HookAfterPreInitUntilStart();
        }
        else
        {
            FEngineLoop::HookAfterPreInit();
        }
        if (void (*Combine)() = FOnlineSubsystemImpl::BeforeFirstDedicatedCheck.exchange(nullptr))
        {
            Log::Warning(L"Server config: the online subsystem never asked whether it is dedicated, merging the server settings now");
            Combine();
        }
        // The MCP services read their configs during PreInit, before the FortMcpConfig hook below, so the server's
        // client permission goes into them here, before anyone can join.
        FFortMcpConfig::AddDedicatedServerPermissionsToLoadedServices();
    }
    else
    {
        FEngineLoop::Hook();
    }
    UGameMapsSettings::Hook();
    UWorld::Hook();
    UNetDriver::Hook();
    AActor::Hook();
    UGameInstance::Hook();
    UFortGameInstance::Hook();
    FOnlineSubsystemImpl::Hook();
    FOnlineVoiceImpl::Hook();
    FFortMcpConfig::Hook();
    UMcpProfileGroup::Hook();
    AFortGameMode::Hook();
    AFortGameModeZone::Hook();
    AFortWorldManager::Hook();
    AFortPlayerController::Hook();
    AFortPlayerState::Hook();
    AFortPlayerControllerOutpost::Hook();
    UFortGlobals::Hook();
    UEngine::Hook();
    UGameEngine::Hook();
    UBuildingStructuralSupportSystem::Hook();
    Diagnostics::HookLobbyServer();
    if (Configuration::Get().IsBattleRoyale())
    {
        UGameMapsSettings::ModeDefaultMap = &AFortGameModeAthena::DefaultMatchURL;
    }
    UFortQuestManager::Hook();
    Hooks::EnableAll();
    if (bHoldCode)
    {
        static constexpr void (*AtStart)() = []
        {
            Hooks::ReleaseCode();
            FEngineLoop::ServerAtStart();
        };
        if (!Packed::RunBeforeEngineStart(AtStart))
        {
            Log::Error(L"Packed exe: patching the code now, before the packer's check on the first ticks, which it may not survive");
            AtStart();
        }
    }

    GameThread::Enqueue([]
    {
        // Battle Royale servers register with the matchmaking service after "Dedicated Server Ready!" (Core-Docs
        // Athena-Boot-1.7.2.md §1 B7).
        if (Configuration::Get().IsBattleRoyale())
        {
            FCommandLine::AddSwitch(L"EnableMMS");
        }
        FCommandLine::RestoreSwitch(L"EnableMMS");
        // The bots-only test match (-CoreBotMatch) has nobody in it but the replay recorder's spectator, whose logout
        // when its upload fails restarts the session ("Logout called on final player"): it records no replay.
        if (Configuration::Get().bBotMatch)
        {
            FCommandLine::AddSwitch(L"noreplays");
        }
        UGameEngine::SetServerTickRate();
        AFortGameModeZone::HookVTables();
        AFortGameModeAthena::HookVTables();
        UFortVisibilityComponent::HookVTables();
        UAbilitySystemComponent::HookVTables();
        UFortKismetLibrary::HookNatives();
        ABuildingSMActor::HookVTables();
        ABuildingContainer::HookVTables();
        ABuildingActor::HookVTables();
        UFortMissionLibrary::HookNatives();
        AFortMission::HookNatives();
        UFortGameplayAbility::HookNatives();
        AFortPlayerController::HookVTables();
        AFortPlayerControllerZone::HookVTables();
        AFortPlayerControllerOutpost::HookVTables();
        AFortPlayerControllerDeployableBase::HookVTables();
        AFortQuickBars::HookVTables();
        AFortPickup::HookVTables();
        AFortObjectiveBase::HookVTables();
        UFortSchematicItem::HookVTables();
        UFortWorldItem::HookVTables();
        UFortNeverPersistItemDefinition::HookVTables();
        AFortMission::HookCallSites();
        AFortMission::HookVTables();
        AFortMissionManager::HookVTables();
        AFortGameMode::HookCallSites();
        AFortWorldManager::HookCallSites();
        UFortMcpProfileWorld::HookVTables();
        AFortGameSession::HookVTables();
        AFortPlayerController::HookReconnectVTables();
        AFortAIGoalManager::HookCallSites();
        AFortAIGoalManager::HookNatives();
        UFortAIEncounterInfo::HookCallSites();
        UFortBTTask_SetFrustrationDiscouragement::HookVTables();
        UFortQueryGenerator_Enemies::HookVTables();
        UFortQueryContext_EncounterPrimaryAssignmentGoals::HookVTables();
        AFortAIPawn::HookVTables();
        ABuildingTrapDefender::HookNatives();
        AFortPlayerPawn::HookVTables();
        AFortPlayerPawn::HookPickupVTables();
        UNetDriver::AddPreReplicationCallback(&AFortPawn::RemoveDestroyedWeaponsFromLists);
        ACharacter::HookCheatMovement();
        ACharacter::OnCheatMovement = &UFortCheatManager::OnCheatMovement;
        UFortCheatManager::HookNatives();
        Finder::ResolveNavigationSystemLive();
        if (Configuration::Get().IsBattleRoyale())
        {
            Finder::ResolveLootTierRedirectLive();
            Finder::ResolvePlaylistDataTablesLive();
            AFortPlayerControllerAthena::HookCallSites();
            AFortPlayerControllerAthena::HookVTables();
            AFortPlayerPawnAthena::HookNatives();
            AFortAthenaSupplyDrop::HookNatives();
            Finder::ResolveAthenaHeroLive();
            ABuildingTrap::HookVTables();
            ABuildingItemCollectorActor::HookVTables();
            AFortGameModeAthena::HookBuildingVTables();
            AFortGameModeAthena::HookCallSites();
            UNetDriver::AddPreReplicationCallback(&AFortPlayerController::ReplicateServerFinishedLoadingAfterGameState);
            UCharacterMovementComponent::HookServerMove(L"/Script/FortniteGame.FortMovementComp_CharacterAthena");
        }
    });

    Log::Info(L"Core V2 startup complete");
}
