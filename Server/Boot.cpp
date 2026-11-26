#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
#include "CrashLog.h"
#include "Diagnostics.h"
#include "Finder.h"
#include "HAL/ThreadHeartBeat.h"
#include "GameMapsSettings.h"
#include "Hooks.h"
#include "LaunchEngineLoop.h"
#include "IPlatformFilePak.h"
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
#include "AI/Navigation/NavCollision.h"
#include "UObject/Linker.h"
#include "UObject/Object.h"
#include "FortAssetManager.h"
#include "FortGameInstance.h"
#include "FortGameMode.h"
#include "FortGameModeZone.h"
#include "Athena/AthenaFillFloor.h"
#include "Athena/FortAthenaExitCraftSpawner.h"
#include "Athena/Mutators/FortAthenaMutator_Ashton.h"
#include "Athena/Mutators/FortAthenaMutator_Wax.h"
#include "Athena/Mutators/FortAthenaMutator_GiveItemsAtGamePhaseStep.h"
#include "Athena/Mutators/FortAthenaMutator_Goose.h"
#include "Athena/FortAthenaSupplyDrop.h"
#include "FortMissionTimerComponent.h"
#include "Missions/FortMissionStormSafeZone.h"
#include "Missions/FortMissionStormShield.h"
#include "Athena/FortAthenaVehicle.h"
#include "Athena/FortOctopusVehicle.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerPawnAthena.h"
#include "Athena/FortPoiManager.h"
#include "Misc/CommandLine.h"
#include "FortVisibilityComponent.h"
#include "FortWorldManager.h"
#include "GameThread.h"
#include "McpProfileGroup.h"
#include "Athena/AI/AthenaAIPerceptionComponent.h"
#include "Camera/FortPlayerCameraZone.h"
#include "Building/BuildingPropMusicPlayer.h"
#include "Building/BuildingProp_DeimosSpawner.h"
#include "Building/BuildingRift.h"
#include "Creative/FortActorOptionsComponent.h"
#include "Creative/FortAthenaCreativePortal.h"
#include "Creative/FortLevelSaveComponent.h"
#include "Creative/FortMinigameSettingsBuilding.h"
#include "Creative/FortVolumeTimeOfDayComponent.h"
#include "Minigame/FortMinigame.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "OnlineSubsystemImpl.h"
#include "OnlineVoiceImpl.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionManager.h"
#include "Online/FortGameSession.h"
#include "Online/FortHotfixManager.h"
#include "Online/FortMcpProfileWorld.h"
#include "AI/FortAIGoalManager.h"
#include "Building/BGAConsumableSpawner.h"
#include "Building/BuildingActor.h"
#include "Building/BuildingContainer.h"
#include "Building/BuildingFoundation.h"
#include "Building/BuildingGameplayActorSpawnMachine.h"
#include "Building/BuildingItemCollectorActor.h"
#include "Minigame/FortMinigameScoreRegistry.h"
#include "Minigame/FortMinigameState.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "Building/BuildingTrap.h"
#include "Building/BuildingTrapDefender.h"
#include "AI/FortBTContext_SuppressGoalUpdate.h"
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
#include "DeployableBases/FortDeployableBaseLibrary.h"
#include "Player/FortPlayerControllerOutpost.h"
#include "Player/FortPlayerControllerPvE.h"
#include "Player/FortPlayerControllerZone.h"
#include "Player/FortPlayerState.h"
#include "FortGlobals.h"
#include "FortKismetLibrary.h"
#include "Abilities/FortAbilitySystemComponent.h"
#include "Abilities/FortGameplayAbility.h"
#include "Abilities/FortGameplayAbilityAthena_PeriodicItemGrant.h"
#include "Missions/FortMissionLibrary.h"
#include "AbilitySystemComponent.h"
#include "VivoxVoiceChatToken.h"
#include "Weapons/FortWeaponRangedMountedCannon.h"

#include <chrono>

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
    // The identity service's Protocol goes back to the shipped https. The launcher's NoRedist\DefaultEngine.ini sets it to
    // http for the client's purchase page (Core-Launcher PurchaseFlowConfig: Protocol://RedirectUrl/exchange), which a
    // server never shows, and from 7.10 (CL-4667333) FBaseServiceMcp::GetBaseUrl (slot 10, 0x4B8BC0) has its config check
    // the URL first (0x60D5A0): unless Protocol is https and ISslCertificateManager::IsDomainPinned(Domain), it calls
    // FPlatformMisc::RequestExit(false), so the server quit as its game mode's auto login built the login URL. CoreRedirect
    // sends the identity service's requests to the backend whatever their scheme.
    // The sidecar service, which USidecarSys's island upload and checkin take their base URL from (7.00 CL-4629139 on,
    // Creative), gets https and a host under .ol.epicgames.com, which CoreRedirect sends to the backend's /api/dss/v1
    // routes; Core-Backend's hotfix sends the same section (services/fortnite/cloudstorage.js sidecarSection). The build
    // ships it as http://127.0.0.1:8000 (Epic's servers had a sidecar process beside them).
    // - 7.00 (#485): with the backend's own address (Protocol=http, Domain=<address>:<port>, the hotfix before #485) the
    //   upload went to "https://127.0.0.1:<port>.ol.epicgames.com/api/dss/v1/...", which libcurl refuses ("Port number
    //   ended with '.'"; LogSidecar "DssUpload failed: errors.com.epicgames.online.generic.connection_failure"), so no
    //   island was saved and every checkout found none.
    // - 7.10 (CL-4667333) and later: the base URL comes from the service's GetBaseUrl (its vtable 0x39B2B38, slot 10 =
    //   0x4B8BE0; 7.20 0x3ACE940, slot 10 = 0x4D7060 with the check at 0x60F0A0), whose check (above) fails both halves of
    //   the shipped URL, so the first island save of a 7.10+ Creative server would quit it. The build pins every host under
    //   .ol.epicgames.com (the pin list is built in, no ini has [SSL] PinnedPublicKeys; account-public-service-prod03.
    //   ol.epicgames.com passes it through that entry).
    constexpr uint32_t FirstSidecarChangelist = 4629139;

    void CombineServerConfig()
    {
        FConfigCacheIni::CombineIntoEngineIni(L"Server config",
            L"[OnlineSubsystem]\r\nbHasVoiceEnabled=true\r\n"
            L"[OnlineSubsystemMcp]\r\nbEnableDefaultServiceTracker=false\r\n"
            L"[OnlineSubsystemMcp.OnlineIdentityMcp]\r\nProtocol=https\r\n"
            L"[XMPP]\r\nbEnabled=false\r\n");
        if (Packed::Changelist() >= FirstSidecarChangelist)
        {
            FConfigCacheIni::CombineIntoEngineIni(L"Sidecar service config",
                L"[OnlineSubsystemMcp.OnlineSidecarServiceMcp]\r\nProtocol=https\r\n"
                L"Domain=mcpsidecar-public-service-prod.ol.epicgames.com\r\n");
        }
    }
}

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Server");
    Log::KeepFromChildProcesses();
    // Which build this is, so a reporter's Server_log says whether a fix was in it: the release workflow passes the run
    // number (the release's build-N tag) and the commit (its first seven hex digits) to the compiler. A local build has
    // neither.
#if defined(CORE_BUILD_NUMBER) && defined(CORE_COMMIT)
    Log::Info(L"Core V2 starting (build-{}, commit {:07x})", CORE_BUILD_NUMBER, static_cast<uint32_t>(CORE_COMMIT));
#else
    Log::Info(L"Core V2 starting (a local build)");
#endif

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
    // The string references were first read as the packer's stub handed over, while its worker threads were still
    // decrypting part of the code; the finders from here on (UFortGameInstance::Init's on 5.40, #385) read them anew.
    if (bAfterPreInit)
    {
        Finder::RescanStringRefs();
    }
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
        // With GIsClient off, before UGameEngine::Init: a window the PreLoadScreen handed the engine gets no viewport.
        UGameEngine::ReleasePreLoadScreenWindow();
        if (void (*Combine)() = FOnlineSubsystemImpl::BeforeFirstDedicatedCheck.exchange(nullptr))
        {
            Log::Warning(L"Server config: the online subsystem never asked whether it is dedicated, merging the server settings now");
            Combine();
        }
        // The MCP services read their configs during PreInit, before the FortMcpConfig hook below, so the server's
        // client permission goes into them here, before anyone can join.
        FFortMcpConfig::AddDedicatedServerPermissionsToLoadedServices();
        // 8.40+ reject Core's server assets pak in PreInit (it carries no signature of Epic's), so it goes in here, before
        // UEngine::Init loads anything from it.
        FPakPlatformFile::MountServerAssets();
    }
    else
    {
        FEngineLoop::Hook();
    }
    UGameMapsSettings::Hook();
    UWorld::Hook();
    // 7.30's Polar Peak ice (CoreV2 #519): Athena_POI_Foundations' level Blueprint calls the stripped
    // SetDynamicFoundationTransform from its BeginPlay, before the game-thread queue below restores it.
    if (Configuration::Get().IsBattleRoyale())
    {
        UWorld::OnBeforeBeginPlay = [](UWorld*) { ABuildingFoundation::HookNatives(); };
    }
    UNetDriver::Hook();
    AActor::Hook();
    UGameInstance::Hook();
    UFortGameInstance::Hook();
    UFortAssetManager::Hook();
    UFortHotfixManager::Hook();
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
    AFortPlayerPawn::Hook();
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

    Finder::ResolveThreadHeartBeat();
    GameThread::Enqueue([]
    {
        // This runs in the engine's first frame with the map (after LoadMap), before its world ticks, and takes seconds
        // (9.10 CL-6639283: about 11 s here, more on a slower PC). The engine's hang detector ends a game thread that sends
        // no heartbeat for HangDuration (25 s; fatal on 9.10, #690), so Core's start-up is held out of it as the engine's
        // own slow work is (FSlowHeartBeatScope).
        const auto Start = std::chrono::steady_clock::now();
        FSlowHeartBeatScope SuspendHeartBeat;
        FCommandLine::CheckServerSwitches();
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
        // Where Core's natives go for the game's Blueprint delegates to call (UClass::AddNativeFunction), before the
        // HookVTables and HookNatives below add theirs.
        Finder::ResolveClassFunctionMap();
        AFortGameModeZone::HookVTables();
        AFortGameModeAthena::HookVTables();
        UFortVisibilityComponent::HookVTables();
        UAbilitySystemComponent::HookVTables();
        UFortAbilitySystemComponent::HookCallSites();
        UFortKismetLibrary::HookNatives();
        ABuildingSMActor::HookVTables();
        ABuildingContainer::HookVTables();
        ABuildingActor::HookVTables();
        UFortMissionLibrary::HookNatives();
        AFortMission::HookNatives();
        AFortMission_RiftSpawners::HookNatives();
        UFortGameplayAbility::HookNatives();
        UFortGameplayAbilityAthena_PeriodicItemGrant::HookNatives();
        AFortPlayerController::HookVTables();
        AFortPlayerControllerZone::HookVTables();
        // Each player's own camera cache (#567), before ServerUpdateCamera and the replication graph read it.
        AFortPlayerCameraZone::HookVTables();
        AFortPlayerControllerPvE::HookVTables();
        AFortPlayerControllerOutpost::HookVTables();
        AFortPlayerControllerDeployableBase::HookVTables();
        UFortDeployableBaseLibrary::HookNatives();
        AFortQuickBars::HookVTables();
        AFortPickup::HookVTables();
        AFortPickup::HookNatives();
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
        // After the two above, which find the EQS slots and the goal checks these need.
        UFortQueryGenerator_AssignmentGoal::HookVTables();
        UFortQueryContext_AllGoals::HookVTables();
        UFortBTContext_SuppressGoalUpdate::HookVTables();
        AFortAIPawn::HookVTables();
        AFortAIPawn::HookNatives();
        ABuildingTrapDefender::HookNatives();
        AFortPlayerPawn::HookVTables();
        AFortPlayerPawn::HookPickupVTables();
        AFortPlayerPawn::HookZiplineVTables();
        AFortPlayerPawn::HookLastEquippedWeaponVTables();
        AFortPlayerPawn::HookNatives();
        AFortPlayerPawn::HookManualReviveNative();
        AFortPawn::HookEmoteNatives();
        UNetDriver::AddPreReplicationCallback(&AFortPawn::RemoveDestroyedWeaponsFromLists);
        ACharacter::HookCheatMovement();
        ACharacter::OnCheatMovement = &UFortCheatManager::OnCheatMovement;
        UFortCheatManager::HookNatives();
        Finder::ResolveNavigationSystemLive();
        // Save the World (#469): the zones' static meshes get the navigation collision a server's have, which the husks'
        // navmeshes are built on. Battle Royale's bots build their own navmesh (UAthenaNavSystem) and keep theirs as it is.
        if (!Configuration::Get().IsBattleRoyale())
        {
            UNavCollision::CreateOnServer();
        }
        // The server-only plugin that gives the game mode its players' Vivox tokens (the "VoiceChatToken" modular
        // feature), which the client builds lack: registered once the backend has shared its Vivox signing.
        FVivoxVoiceChatTokenModule::StartupModule();
        // The supply drop's stripped pickup natives: Battle Royale's crates and Supply Llamas, and Save the World's Frostnite
        // supply drops (EndlessModeSupplyDrop_C is an AthenaSupplyDrop_C).
        AFortAthenaSupplyDrop::HookNatives();
        // 9.00's Wargames: the Time Cheats' wave timer and the Safety Zone's generator bubbles (#655).
        UFortMissionTimerComponent::HookNatives();
        AFortMissionStormShield::HookNatives();
        AFortMissionStormSafeZone::HookNatives();
        if (Configuration::Get().IsBattleRoyale())
        {
            // The Athena AI controllers' sight (#605), before the first one is made.
            UAthenaAIPerceptionComponent::EnableServerSenses();
            Finder::ResolveLootTierRedirectLive();
            Finder::ResolvePlaylistDataTablesLive();
            AFortPlayerControllerAthena::HookCallSites();
            AFortPlayerControllerAthena::HookVTables();
            AFortPlayerPawnAthena::HookNatives();
            AFortPlayerPawnAthena::HookVTables();
            AFortAthenaExitCraftSpawner::HookNatives();
            ABuildingFoundation::HookNatives();
            ABGAConsumableSpawner::HookNatives();
            AFortPoiManager::HookNatives();
            AFortAthenaVehicle::HookVTables();
            // 8.40's Air Royale: the mutator's planes and its floating upgrades (#609, #611).
            AFortAthenaMutator_Goose::HookNatives();
            // 8.50's Endgame: the stone capture, the gauntlet's give, the team loadouts and the death removals.
            AFortAthenaMutator_Ashton::HookNatives();
            // 9.01's Wick's Bounty: the bots' pawns told of as a client's loaded pawn is (the token payout on death).
            AFortAthenaMutator_Wax::HookNatives();
            // 8.00+ LTM items: the give-items mutators' natives (Food Fight's glider and grappler, Gun Game, Air Royale; #643).
            AFortAthenaMutator_GiveItemsAtGamePhaseStep::HookNatives();
            // 8.40's Food Fight: Deep Fried: the lava stops at the mascots (#614).
            AAthenaFillFloor::HasSetCapHeight();
            // 8.30's Pirate Cannon: firing the passenger (ServerFireActorInCannon_Implementation, stripped).
            AFortWeaponRangedMountedCannon::HookVTables();
            AFortOctopusVehicle::HookCallSites();
            Finder::ResolveAthenaHeroLive();
            ABuildingTrap::HookVTables();
            ABuildingItemCollectorActor::HookVTables();
            // 8.30's Reboot Van: the reboot its interaction's timer runs.
            ABuildingGameplayActorSpawnMachine::HookNatives();
            AFortMinigameScoreRegistry::HookNatives();
            AFortMinigameState::HookNatives();
            // 7.00's Creative: the rifts, the islands' load and save, the Creative RPCs and the minigames' start.
            AFortAthenaCreativePortal::HookNatives();
            AFortPlayerControllerAthena::HookCreativeVTables();
            // 7.10's island codes, publishing and backups (#488).
            AFortPlayerControllerAthena::HookCreativeIslandTools();
            // 7.30's island edit permissions: the players on the island told of a change (the Creative phone).
            UFortLevelSaveComponent::HookPermissions();
            AFortMinigame::HookNatives();
            // Creative's creature spawner (8.00 on, #566): its rift, the rift's RiftCreated event and the settings its knobs pick.
            ABuildingProp_DeimosSpawner::HookVTables();
            // 7.40+ Creative: the radio's track and playback options (StartSongForOptionsChange, stripped).
            ABuildingPropMusicPlayer::HookNatives();
            // 9.10 Creative: device and prop options (SetPropertyOverrideValues, stripped).
            UFortActorOptionsComponent::HookCallSites();
            AAthenaCreativeRift::HookVTables();
            // 8.00+ Creative (#596): an island's Time of Day setting, its own time of day manager.
            UFortVolumeTimeOfDayComponent::HookVTables();
            // 9.00+ Creative (#684): the machine's SettingsVolume, which links it to its volume on the clients (the GAME tab).
            AFortMinigameSettingsBuilding::HookVTables();
            AFortGameModeAthena::HookBuildingVTables();
            AFortGameModeAthena::HookCallSites();
            AFortGameStateAthena::HookCallSites();
            // 8.20+: the server's team and squad maps from the game members Core adds (CoreV2 #586), found now for the log.
            if (AFortGameStateAthena::HasGameMemberInfoArray())
            {
                Finder::ResolveGameMemberAddedLive(FGameMemberInfoArray::OwningGameStateOffset());
            }
            AFortGameModeAthena::HookMatchAssignmentVTables();
            UNetDriver::AddPreReplicationCallback(&AFortPlayerController::ReplicateServerFinishedLoadingAfterGameState);
            UCharacterMovementComponent::HookServerMove(L"/Script/FortniteGame.FortMovementComp_CharacterAthena");
        }
        if (SuspendHeartBeat.IsSuspended())
        {
            Log::Info(L"Core's start-up on the game thread took {} ms with the hang detector's heartbeat suspended",
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Start).count());
        }
    });

    Log::Info(L"Core V2 startup complete");
}
