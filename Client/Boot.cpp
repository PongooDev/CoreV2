#include "pch.h"
#include "Boot.h"
#include "Building/BuildingSMActor.h"
#include "CrashLog.h"
#include "Diagnostics.h"
#include "Finder.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Log.h"
#include "Packed.h"
#include "Engine/Console.h"
#include "Engine/GameEngine.h"
#include "Engine/GameViewportClient.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "Misc/ConfigCacheIni.h"
#include "OnlineVoiceImpl.h"
#include "Player/FortCheatManager.h"
#include "Player/FortPlayerController.h"
#include "UI/AthenaHUDContext.h"
#include "UI/FortAsyncAction_ShowPartyDialog.h"
#include "UI/FortPartyContext.h"
#include "UI/FortUITeamInfo.h"

static void Start();

void Boot::Initialize()
{
    Log::Initialize(L"Client");
    Log::Info(L"Core Client starting");

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
    // Packed builds only: the packer checks its decrypted code as the initial load ends, and patches
    // present at that moment hang the game, so the detours go in once the engine is up.
    Packed::WaitForCodeCheck();
    Hooks::EnableAll();

    GameThread::Enqueue([]
    {
        APlayerController::Hook();
        AFortPlayerController::HookVoice();
        AFortPlayerController::HookClientNatives();
        UFortAsyncAction_ShowPartyDialog::HookNatives();
        UFortPartyContext::StartAthenaPartyBar();
        ACharacter::HookCheatMovement();
        ACharacter::OnCheatMovement = &UFortCheatManager::OnCheatMovement;
        UFortCheatManager::StartDisguiseView();
        UFortCheatManager::StartInfiniteMagazineView();
        UFortCheatManager::StartSpeedView();
        ABuildingSMActor::HookClientVTables();
    });

    Log::Info(L"Core Client startup complete");
}
