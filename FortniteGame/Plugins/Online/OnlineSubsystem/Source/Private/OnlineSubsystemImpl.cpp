#include "pch.h"
#include "OnlineSubsystemImpl.h"
#include "Hooks.h"
#include "Natives.h"

void FOnlineSubsystemImpl::Hook()
{
    Hooks::Detour(L"FOnlineSubsystemImpl::IsDedicated", reinterpret_cast<void*>(Natives::FOnlineSubsystemImpl_IsDedicated), &IsDedicatedHK, nullptr);
}

bool FOnlineSubsystemImpl::IsDedicatedHK(const FOnlineSubsystemImpl* This)
{
    return This->IsDedicated();
}
