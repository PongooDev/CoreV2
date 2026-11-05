#include "pch.h"
#include "FortGameInstance.h"
#include "Engine/Engine.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Packed.h"

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
    // UGameInstance::Init runs inside UEngine::Init, before a 3.5+ build's packer checks the code on the first ticks
    // (Packed::ChecksCodeOnFirstTicks), when Core holds its detours back. It is virtual, so those builds swap it in the
    // vtables that hold it, data the check doesn't look at.
    if (Packed::ChecksCodeOnFirstTicks())
    {
        const std::vector<void**> Slots = Finder::ReadOnlySlotsHolding(Init);
        if (Slots.empty())
        {
            Log::Error(L"Finder: UFortGameInstance::Init is in no vtable");
            return;
        }
        InitOG = reinterpret_cast<decltype(InitOG)>(Init);
        for (void** Slot : Slots)
        {
            Hooks::SwapVTable(L"UFortGameInstance::Init", Slot, 0, reinterpret_cast<void*>(&InitHK));
        }
        return;
    }
    Hooks::Detour(L"UFortGameInstance::Init", reinterpret_cast<void*>(Init), &InitHK, &InitOG);
}

void UFortGameInstance::InitHK(UFortGameInstance* This)
{
    // On 3.5+ Init runs before Core's UGameInstance::IsDedicatedServerInstance detour is in, and makes the server's
    // analytics provider (which the game mode's session id change reports to) only for a dedicated server instance, so
    // the instance is marked as one the way IsDedicatedServerInstance reads it, in its world context.
    if (Packed::ChecksCodeOnFirstTicks() && This->WorldContext)
    {
        This->WorldContext->RunAsDedicated = true;
    }
    This->Init();
}
