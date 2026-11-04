#include "pch.h"
#include "Engine/GameEngine.h"
#include "GameFramework/GameUserSettings.h"
#include "GameFramework/PlayerController.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"

namespace
{
    // The server's frame rate (#109): about the 32 Hz a server reached for a 60 FPS limit, held steady.
    constexpr float ServerTickRate = 30.0f;
}

void UGameEngine::TickHK(UGameEngine* This, float DeltaSeconds, bool bIdleMode)
{
    GameThread::Tick();
#if !UE_SERVER
    APlayerController::EnsureLocalCheatManager(This->GameInstance);
#endif
    TickOG(This, DeltaSeconds, bIdleMode);
}

void UGameEngine::Hook()
{
    for (uint8_t* Check : Sites::GameEngineTickDedicatedServerChecks)
    {
        Hooks::SkipBranch(L"UGameEngine::Tick IsRunningDedicatedServer", Check);
    }
    Hooks::Detour(L"UGameEngine::Tick", reinterpret_cast<void*>(Natives::UGameEngine_Tick), &TickHK, &TickOG);
}

void UGameEngine::SetServerTickRate()
{
    // UGameEngine::GetMaxTickRate (1.8 0x2178980, 1.7.2 0x2105800) gives a dedicated server its NetServerMaxTickRate
    // (Fortnite's ini: 20) unless UEngine's (1.8 0x24D03F0) has a rate, and that returns t.MaxFPS whenever it is set. The
    // server is the client's exe and applies the player's own GameUserSettings at start-up, so t.MaxFPS was their
    // FrameRateLimit: the server aimed at 60 frames a second for a 60 FPS limit, more for a higher one, 20 for an
    // unlimited one (#109). The server's own copy of the settings gets ServerTickRate as its frame rate limit, applied
    // the way the settings screen applies one (ApplyNonResolutionSettings sets t.MaxFPS from it) once the engine has
    // applied the player's, and is never saved, so the player's GameUserSettings.ini keeps their limit. The launcher's
    // servers have their own saved folder (-saveddirsuffix, Core-Launcher ServerConfig) and so their own settings, but a
    // server started without one still reads the player's.
    GameThread::Enqueue([]
    {
        UGameUserSettings* Settings = UGameUserSettings::GetGameUserSettings();
        if (!Settings)
        {
            Log::Error(L"Server tick rate: no GameUserSettings, the server keeps the player's frame rate limit");
            return;
        }
        Settings->SetFrameRateLimit(ServerTickRate);
        Settings->ApplyNonResolutionSettings();
        Log::Info(L"Server tick rate: {:.0f} Hz (frame rate limit {:.0f})", ServerTickRate, Settings->GetFrameRateLimit());
    });
}
