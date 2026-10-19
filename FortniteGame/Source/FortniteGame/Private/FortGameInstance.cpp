#include "pch.h"
#include "FortGameInstance.h"
#include "Hooks.h"
#include "Natives.h"

void UFortGameInstance::Hook()
{
    if (uint8_t* Check = Sites::FortGameInstanceStartDedicatedCheck)
    {
        const uint8_t Skip[] = { 0xEB, static_cast<uint8_t>(Sites::FortGameInstanceStartDedicatedTarget - (Check + 2)) };
        Hooks::WriteBytes(L"UFortGameInstance::StartGameInstance IsDedicatedServerInstance", Check, Skip);
    }
}
