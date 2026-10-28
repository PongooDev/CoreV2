#include "pch.h"
#include "Boot.h"
#include "Building/BuildingSMActor.h"
#include "CrashLog.h"
#include "Diagnostics.h"
#include "Finder.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Log.h"
#include "Engine/Console.h"
#include "Engine/GameEngine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Misc/ConfigCacheIni.h"
#include "OnlineVoiceImpl.h"
#include "Player/FortPlayerController.h"
#include "UI/AthenaHUDContext.h"
#include "UI/FortAsyncAction_ShowPartyDialog.h"
#include "UI/FortUITeamInfo.h"

void Boot::Initialize()
{
    Log::Initialize(L"Client");
    Log::Info(L"Core Client starting");
    CrashLog::Install();

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
    UGameEngine::Hook();
    Diagnostics::HookLobbyClient();
    Diagnostics::HookQuestTiles();
    // Before the engine reads its config: the voice module and the online subsystem's voice interface start with it.
    FConfigCacheIni::EnableVoice(true);
    FOnlineVoiceImpl::Hook();
    UFortUITeamInfo::Hook();
    UAthenaHUDContext::Hook();
    Hooks::EnableAll();

    GameThread::Enqueue([]
    {
        APlayerController::Hook();
        AFortPlayerController::HookVoice();
        AFortPlayerController::HookClientNatives();
        UFortAsyncAction_ShowPartyDialog::HookNatives();
        ACharacter::HookCheatMovement();
        ABuildingSMActor::HookClientVTables();
    });

    Log::Info(L"Core Client startup complete");
}
