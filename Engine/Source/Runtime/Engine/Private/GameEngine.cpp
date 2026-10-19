#include "pch.h"
#include "Engine/GameEngine.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Natives.h"

void UGameEngine::TickHK(UGameEngine* This, float DeltaSeconds, bool bIdleMode)
{
    GameThread::Tick();
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
