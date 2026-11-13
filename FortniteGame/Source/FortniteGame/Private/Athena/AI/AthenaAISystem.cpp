#include "pch.h"
#include "Athena/AI/AthenaAISystem.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Log.h"
#include "Templates/Casts.h"

UAthenaAISystem* UAthenaAISystem::Get(const UWorld* World)
{
    UObject* const AISystem = World ? reinterpret_cast<UObject*>(World->GetAISystem()) : nullptr;
    return AISystem && StaticClass() ? Cast<UAthenaAISystem>(AISystem) : nullptr;
}

void UAthenaAISystem::OnAISettingsUpdated(const UObject* Settings)
{
#if WITH_SERVER_CODE
    UAthenaAIPerceptionManager* const Manager = PerceptionManager;
    if (!Settings || !Manager || !Finder::ResolveAthenaAIPerceptionLive())
    {
        return;
    }
    Manager->OnAISettingsUpdated(Settings);
    Log::Info(L"Battle Royale AI: {}'s perception manager takes {}'s footstep hearing (the players' footsteps reach the AI's hearing from now on)",
        GetName().ToStdString(), Settings->GetName().ToStdString());
#endif
}
