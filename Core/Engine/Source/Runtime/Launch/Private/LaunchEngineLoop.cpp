#include "pch.h"
#include "LaunchEngineLoop.h"
#include "Hooks.h"
#include "Natives.h"

void FEngineLoop::Hook()
{
    uint8_t* RegularClient = Sites::PreInitRegularClient;
    if (!RegularClient)
    {
        return;
    }

    const uint8_t IsRegularClient[] = { 0x41, 0xB5, 0x00 };
    const uint8_t IsServer[] = { 0xC6, 0x05, RegularClient[6], RegularClient[7], RegularClient[8], RegularClient[9], 0x01 };
    Hooks::WriteBytes(L"FEngineLoop::PreInit bIsRegularClient", RegularClient, IsRegularClient);
    Hooks::WriteBytes(L"FEngineLoop::PreInit GIsServer", RegularClient + 3, IsServer);
}
