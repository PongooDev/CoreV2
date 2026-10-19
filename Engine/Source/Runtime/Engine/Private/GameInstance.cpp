#include "pch.h"
#include "Engine/GameInstance.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "Hooks.h"
#include "Natives.h"

bool UGameInstance::IsDedicatedServerInstance() const
{
    if (IsRunningDedicatedServer())
    {
        return true;
    }

    return WorldContext ? WorldContext->RunAsDedicated : false;
}

void UGameInstance::Hook()
{
    Hooks::Detour(L"UGameInstance::IsDedicatedServerInstance", reinterpret_cast<void*>(Natives::UGameInstance_IsDedicatedServerInstance), &IsDedicatedServerInstanceHK, nullptr);
}

bool UGameInstance::IsDedicatedServerInstanceHK(const UGameInstance* This)
{
    return This->IsDedicatedServerInstance();
}
