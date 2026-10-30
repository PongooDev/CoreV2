#include "pch.h"
#include "OnlineSubsystemImpl.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"

void FOnlineSubsystemImpl::Hook()
{
    Hooks::Detour(L"FOnlineSubsystemImpl::IsDedicated", reinterpret_cast<void*>(Natives::FOnlineSubsystemImpl_IsDedicated), &IsDedicatedHK, nullptr);
}

void FOnlineSubsystemImpl::HookVTables()
{
    const std::vector<void**> Slots = Finder::ReadOnlySlotsHolding(reinterpret_cast<uintptr_t>(Natives::FOnlineSubsystemImpl_IsDedicated));
    if (Slots.empty())
    {
        Log::Error(L"FOnlineSubsystemImpl::IsDedicated is in no vtable, the online subsystem starts as a client's");
        return;
    }
    bRecordSubsystems = true;
    for (void** Slot : Slots)
    {
        Hooks::SwapVTable(L"FOnlineSubsystemImpl::IsDedicated", Slot, 0, reinterpret_cast<void*>(&IsDedicatedHK));
    }
}

void FOnlineSubsystemImpl::Remember(const FOnlineSubsystemImpl* Subsystem)
{
    for (std::atomic<const FOnlineSubsystemImpl*>& Slot : Subsystems)
    {
        const FOnlineSubsystemImpl* Expected = nullptr;
        if (Slot.load() == Subsystem || Slot.compare_exchange_strong(Expected, Subsystem) || Expected == Subsystem)
        {
            return;
        }
    }
}

bool FOnlineSubsystemImpl::IsDedicatedHK(const FOnlineSubsystemImpl* This)
{
    if (void (*Callback)() = BeforeFirstDedicatedCheck.exchange(nullptr))
    {
        Callback();
    }
    if (bRecordSubsystems)
    {
        Remember(This);
    }
    return This->IsDedicated();
}
