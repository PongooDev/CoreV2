#include "pch.h"
#include "Engine/GameEngine.h"
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

float UGameEngine::GetMaxTickRateHK(const UGameEngine* This, float DeltaTime, bool bAllowFrameRateSmoothing)
{
    return ServerTickRate;
}

void UGameEngine::HookServerTickRate()
{
    // UGameEngine::GetMaxTickRate (1.8 0x2178980, 1.7.2 0x2105800) gives a dedicated server its NetServerMaxTickRate
    // (Fortnite's ini: 20) unless UEngine's (1.8 0x24D03F0) has a rate, and that returns t.MaxFPS whenever it is set. The
    // server is the client's exe and applies the player's own GameUserSettings, so t.MaxFPS was their FrameRateLimit: the
    // server aimed at 60 frames a second for a 60 FPS limit, more for a higher one, 20 for an unlimited one, and the wait
    // ending on the system tick made 60 about 32. The server now aims at ServerTickRate whatever the player's settings are.
    UEngine* Engine = GEngine;
    void** VTable = Engine ? *reinterpret_cast<void***>(Engine) : nullptr;
    if (!VTable || Natives::UEngine_GetMaxTickRateIndex < 0)
    {
        Log::Error(L"Server tick rate: no engine or UEngine::GetMaxTickRate slot, the server keeps the client's frame rate limit");
        return;
    }
    if (Hooks::SwapVTable(L"UEngine::GetMaxTickRate", VTable, Natives::UEngine_GetMaxTickRateIndex, reinterpret_cast<void*>(&GetMaxTickRateHK)))
    {
        Log::Info(L"Server tick rate: {:.0f} Hz", ServerTickRate);
    }
}
