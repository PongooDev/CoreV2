#include "pch.h"
#include "Boot.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Engine/Console.h"
#include "Engine/GameViewportClient.h"

void Boot::Initialize()
{
    Log::Initialize(L"Client");
    Log::Info(L"Core Client starting");

    if (MH_Initialize() != MH_OK)
    {
        Log::Error(L"MinHook failed to initialize");
        return;
    }

    if (!Finder::ResolveRuntime() || !Finder::ResolveClient())
    {
        Log::Error(L"Startup aborted: required addresses are missing");
        return;
    }

    UGameViewportClient::Hook();
    UConsole::Hook();
    Hooks::EnableAll();

    Log::Info(L"Core Client startup complete");
}
