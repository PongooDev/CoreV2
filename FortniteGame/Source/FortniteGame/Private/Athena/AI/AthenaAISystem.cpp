#include "pch.h"
#include "Athena/AI/AthenaAISystem.h"
#include "Athena/AI/AthenaAIController.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Log.h"
#include "Templates/Casts.h"

UAthenaAISystem* UAthenaAISystem::Get(const UWorld* World)
{
    UObject* const AISystem = World ? reinterpret_cast<UObject*>(World->GetAISystem()) : nullptr;
    return AISystem && StaticClass() ? Cast<UAthenaAISystem>(AISystem) : nullptr;
}

bool UAthenaAISystem::IsManagedController(const AAthenaAIController* Controller) const
{
    for (const AAthenaAIController* Managed : GetManagedControllers())
    {
        if (Managed == Controller)
        {
            return true;
        }
    }
    return false;
}

void UAthenaAISystem::OnAISettingsUpdated(const UObject* Settings)
{
#if WITH_SERVER_CODE
    // The match's AI settings are what brings its Athena AI (the AI director they name), so its controllers are watched
    // from here: each takes itself out of this system's lists as it is destroyed (#447).
    if (Settings)
    {
        AAthenaAIController::BindDestroyedForWorld(Cast<UWorld>(GetOuter()));
    }
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
