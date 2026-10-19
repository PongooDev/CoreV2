#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
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
#include "OnlineSubsystemImpl.h"

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
    AFortGameMode::Hook();
    UEngine::Hook();
    UGameEngine::Hook();
    Hooks::EnableAll();

    Log::Info(L"Core V2 startup complete");
}
