#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
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
#include "Engine/World.h"
#include "UObject/Linker.h"
#include "UObject/Object.h"
#include "FortGameInstance.h"
#include "FortGameMode.h"
#include "FortGameModeZone.h"
#include "FortWorldManager.h"
#include "GameThread.h"
#include "McpProfileGroup.h"
#include "Online/NotForLicensees/FortMcpConfig.h"
#include "OnlineSubsystemImpl.h"
#include "Items/FortPickup.h"
#include "Missions/FortMission.h"
#include "Missions/FortMissionManager.h"
#include "Missions/FortObjectiveBase.h"
#include "Items/FortSchematicItem.h"
#include "Items/FortQuickBars.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "FortGlobals.h"
#include "FortKismetLibrary.h"
#include "AbilitySystemComponent.h"

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Server");
    Log::Info(L"Core V2 starting");

    if (MH_Initialize() != MH_OK)
    {
        Log::Error(L"MinHook failed to initialize");
        return;
    }

    if (!Finder::ResolveRuntime() || !Finder::ResolveServer())
    {
        Log::Error(L"Startup aborted: required addresses are missing");
        return;
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
    FFortMcpConfig::Hook();
    UMcpProfileGroup::Hook();
    AFortGameMode::Hook();
    AFortGameModeZone::Hook();
    AFortWorldManager::Hook();
    AFortPlayerController::Hook();
    AFortPlayerState::Hook();
    UFortGlobals::Hook();
    UEngine::Hook();
    UGameEngine::Hook();
    Diagnostics::HookLobbyServer();
    Hooks::EnableAll();

    GameThread::Enqueue([]
    {
        AFortGameModeZone::HookVTables();
        UAbilitySystemComponent::HookVTables();
        UFortKismetLibrary::HookNatives();
        AFortPlayerController::HookVTables();
        AFortQuickBars::HookVTables();
        AFortPickup::HookVTables();
        AFortObjectiveBase::HookVTables();
        UFortSchematicItem::HookVTables();
        UFortWorldItem::HookVTables();
        AFortMission::HookCallSites();
        AFortMissionManager::HookVTables();
    });

    Log::Info(L"Core V2 startup complete");
}
