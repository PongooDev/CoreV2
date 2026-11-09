#include "pch.h"
#include "LaunchEngineLoop.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"

namespace
{
    void HookIdleMode()
    {
        if (uint8_t* IdleCheck = Sites::ShouldUseIdleModeCheck)
        {
            uint8_t NotWindowed[7] = { 0xE9, 0, 0, 0, 0, 0x90, 0x90 };
            *reinterpret_cast<int32_t*>(NotWindowed + 1) = static_cast<int32_t>(Sites::ShouldUseIdleModeFalse - (IdleCheck + 5));
            Hooks::WriteBytes(L"FEngineLoop::ShouldUseIdleMode SupportsWindowedMode", IdleCheck, NotWindowed);
        }
        else if (uint8_t* IdleBranch = Sites::ShouldUseIdleModeInlinedBranch)
        {
            Hooks::SkipBranch(L"FEngineLoop::Tick ShouldUseIdleMode t.IdleWhenNotForeground", IdleBranch);
        }
    }
}

void FEngineLoop::Hook()
{
    uint8_t* RegularClient = Sites::PreInitRegularClient;
    if (!RegularClient)
    {
        return;
    }

    const uint8_t IsServer[] = { 0xC6, 0x05, RegularClient[6], RegularClient[7], RegularClient[8], RegularClient[9], 0x01 };
    const uint8_t IsClient[] = { 0xC6, 0x05, RegularClient[13], RegularClient[14], RegularClient[15], RegularClient[16], 0x00 };
    Hooks::WriteBytes(L"FEngineLoop::PreInit GIsServer", RegularClient + 3, IsServer);
    Hooks::WriteBytes(L"FEngineLoop::PreInit GIsClient", RegularClient + 10, IsClient);

    for (uint8_t* Check : Sites::PreInitDedicatedServerChecks)
    {
        Hooks::SkipBranch(L"FEngineLoop::PreInit IsRunningDedicatedServer", Check);
    }

    HookIdleMode();
}

void FEngineLoop::HookAfterPreInit()
{
    if (!Natives::GIsServer || !Natives::GIsClient)
    {
        Log::Error(L"FEngineLoop: GIsServer or GIsClient not found, the engine stays a client");
    }
    else
    {
        *Natives::GIsServer = true;
        *Natives::GIsClient = false;
        Log::Info(L"FEngineLoop: GIsServer on, GIsClient off (after PreInit)");
    }

    HookIdleMode();
}

void FEngineLoop::HookAfterPreInitUntilStart()
{
    if (!Natives::GIsServer || !Natives::GIsClient)
    {
        Log::Error(L"FEngineLoop: GIsServer or GIsClient not found, the engine stays a client");
    }
    else
    {
        *Natives::GIsClient = false;
        Log::Info(L"FEngineLoop: GIsClient off (after PreInit), GIsServer on once the engine starts");
    }

    HookIdleMode();
}

void FEngineLoop::ServerAtStart()
{
    if (Natives::GIsServer)
    {
        *Natives::GIsServer = true;
        Log::Info(L"FEngineLoop: GIsServer on (engine start)");
    }
}
