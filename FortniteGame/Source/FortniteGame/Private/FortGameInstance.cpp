#include "pch.h"
#include "FortGameInstance.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"

void UFortGameInstance::Init()
{
    InitOG(this);

#if UE_SERVER
    TooltipManager = nullptr;
#endif
}

void UFortGameInstance::Hook()
{
    if (uint8_t* Check = Sites::FortGameInstanceStartDedicatedCheck)
    {
        const uint8_t Skip[] = { 0xEB, static_cast<uint8_t>(Sites::FortGameInstanceStartDedicatedTarget - (Check + 2)) };
        Hooks::WriteBytes(L"UFortGameInstance::StartGameInstance IsDedicatedServerInstance", Check, Skip);
    }

    uintptr_t Init = 0;
    for (uintptr_t Reference : Finder::StringRefs(L"TeamChatroom"))
    {
        Init = Init ? Init : Finder::FunctionStart(Reference);
    }
    if (!Init)
    {
        Log::Error(L"Finder: UFortGameInstance::Init not found");
        return;
    }
    Hooks::Detour(L"UFortGameInstance::Init", reinterpret_cast<void*>(Init), &InitHK, &InitOG);
}

void UFortGameInstance::InitHK(UFortGameInstance* This)
{
    This->Init();
}
