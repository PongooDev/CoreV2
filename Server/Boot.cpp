#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
#include "Finder.h"
#include "GameMapsSettings.h"
#include "Hooks.h"
#include "LaunchEngineLoop.h"
#include "Log.h"
#include "Engine/GameEngine.h"
#include "Engine/World.h"

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Server", Configuration::Get().bConsole);
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

    FEngineLoop::Hook();
    UGameMapsSettings::Hook();
    UWorld::Hook();
    UGameEngine::Hook();
    Hooks::EnableAll();

    Log::Info(L"Core V2 startup complete");
}
