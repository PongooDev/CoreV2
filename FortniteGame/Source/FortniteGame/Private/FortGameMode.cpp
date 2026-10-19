#include "pch.h"
#include "FortGameMode.h"
#include "CoreGlobals.h"
#include "Hooks.h"
#include "Natives.h"
#include "Online/FortGameSessionDedicated.h"

TSubclassOf<AGameSession> AFortGameMode::GetGameSessionClass() const
{
    if (IsRunningDedicatedServer())
    {
        return AFortGameSessionDedicated::StaticClass();
    }

    return AFortGameSession::StaticClass();
}

void AFortGameMode::Hook()
{
    Hooks::Detour(L"AFortGameMode::GetGameSessionClass", reinterpret_cast<void*>(Natives::AFortGameMode_GetGameSessionClass), &GetGameSessionClassHK, nullptr);
}

TSubclassOf<AGameSession>* AFortGameMode::GetGameSessionClassHK(const AFortGameMode* This, TSubclassOf<AGameSession>* Result)
{
    *Result = This->GetGameSessionClass();
    return Result;
}
