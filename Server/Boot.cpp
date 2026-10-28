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
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
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
#include "Online/FortMcpProfileWorld.h"
#include "AI/FortAIGoalManager.h"
#include "Building/BuildingContainer.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "Building/BuildingTrap.h"
#include "AI/FortBTTask_SetFrustrationDiscouragement.h"
#include "AI/FortQueryContexts.h"
#include "AI/FortQueryGenerators.h"
#include "Missions/FortObjectiveBase.h"
#include "Items/FortSchematicItem.h"
#include "Items/FortQuickBars.h"
#include "Pawns/FortAIPawn.h"
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

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Server");
    Log::Info(L"Core V2 starting");
    CrashLog::Install();
    FCommandLine::ApproveServerSwitches();
    HonourTimerResolution();

    // Without its hooks the process would carry on as a game client (a window and a front end, Core-Launcher #4), which
    // the launcher would keep counting as a server: a server that can't start stops.
    if (MH_Initialize() != MH_OK)
    {
        Log::Fatal(L"MinHook failed to initialize");
    }

    if (!Finder::ResolveRuntime() || !Finder::ResolveServer())
    {
        Log::Fatal(L"Startup aborted: required addresses are missing");
    }

    FApp::Hook();
    FConfigCacheIni::Hook();
    UObject::Hook();
    FLinker::Hook();
    FEngineLoop::Hook();
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
        UFortQuestManager::Hook();
    }
    Hooks::EnableAll();

    GameThread::Enqueue([]
    {
        // Battle Royale servers register with the matchmaking service after "Dedicated Server Ready!" (Core-Docs
        // Athena-Boot-1.7.2.md §1 B7).
        if (Configuration::Get().IsBattleRoyale())
        {
            FCommandLine::AddSwitch(L"EnableMMS");
        }
        FCommandLine::RestoreSwitch(L"EnableMMS");
        UGameEngine::SetServerTickRate();
        AFortGameModeZone::HookVTables();
        AFortGameModeAthena::HookVTables();
        UFortVisibilityComponent::HookVTables();
        UAbilitySystemComponent::HookVTables();
        UFortKismetLibrary::HookNatives();
        ABuildingSMActor::HookVTables();
        ABuildingContainer::HookVTables();
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
        AFortAIGoalManager::HookCallSites();
        AFortAIGoalManager::HookNatives();
        UFortAIEncounterInfo::HookCallSites();
        UFortBTTask_SetFrustrationDiscouragement::HookVTables();
        UFortQueryGenerator_Enemies::HookVTables();
        UFortQueryContext_EncounterPrimaryAssignmentGoals::HookVTables();
        AFortAIPawn::HookVTables();
        ACharacter::HookCheatMovement();
        UFortCheatManager::HookNatives();
        Finder::ResolveNavigationSystemLive();
        if (Configuration::Get().IsBattleRoyale())
        {
            Finder::ResolveLootTierRedirectLive();
            AFortPlayerControllerAthena::HookCallSites();
            Finder::ResolveAthenaHeroLive();
            ABuildingTrap::HookVTables();
            AFortGameModeAthena::HookBuildingVTables();
            AFortGameModeAthena::HookCallSites();
            UCharacterMovementComponent::HookServerMove(L"/Script/FortniteGame.FortMovementComp_CharacterAthena");
        }
    });

    Log::Info(L"Core V2 startup complete");
}
