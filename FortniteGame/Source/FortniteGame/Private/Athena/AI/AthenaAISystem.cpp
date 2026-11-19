#include "pch.h"
#include "Athena/AI/AthenaAISystem.h"
#include "Athena/AI/AthenaAIController.h"
#include "Delegates/Delegate.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"

#include <vector>

namespace
{
    // The systems whose lists each garbage collection's start clears of pending-kill controllers (one per match world;
    // a world's goes once its world is torn down, when the collector no longer walks its lists).
    std::vector<FWeakObjectPtr> WatchedSystems;
}

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
        WatchGarbageCollection();
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

int32_t UAthenaAISystem::UnregisterPendingKillControllers(int32_t& Twice)
{
    Twice = 0;
    int32_t Unregistered = 0;
#if WITH_SERVER_CODE
    if (!Natives::UAthenaAISystem_UnregisterManagedController)
    {
        return 0;
    }
    // The collector clears the entries of pending-kill controllers when it marks what is reachable (AIControllers is a
    // UPROPERTY), and AAthenaAIController::BeginDestroy's unregister then swaps a cleared entry into the place it frees
    // and writes the moved controller's index through it (#447, #419). Here, before that, the build's own unregister
    // takes them out while each entry still points at its controller. From the end down, so what a RemoveAtSwap brings
    // into a place is an entry already looked at; each place is looked at again until it holds a live controller.
    const TArray<AAthenaAIController*>& Controllers = GetManagedControllers();
    for (int32_t Index = Controllers.Num() - 1; Index >= 0; --Index)
    {
        while (Index < Controllers.Num())
        {
            AAthenaAIController* Controller = Controllers[Index];
            if (!Controller || !Controller->IsPendingKill() || Get(Controller->GetWorld()) != this)
            {
                break;
            }
            // The unregister takes out the entry the controller's index points at. An entry it doesn't point at (the
            // controller was registered again while still listed) is taken out by pointing the index at it first.
            const bool bSecondEntry = AAthenaAIController::HasManagedTickIndex() && Controller->ManagedTickIndex != Index;
            if (bSecondEntry)
            {
                Controller->ManagedTickIndex = Index;
            }
            const int32_t NumBefore = Controllers.Num();
            Natives::UAthenaAISystem_UnregisterManagedController(Controller);
            if (Controllers.Num() != NumBefore - 1)
            {
                break;
            }
            ++Unregistered;
            Twice += bSecondEntry ? 1 : 0;
        }
    }
#endif
    return Unregistered;
}

void UAthenaAISystem::HandlePreGarbageCollect()
{
#if WITH_SERVER_CODE
    static int32_t Logged = 0;
    for (size_t Watched = 0; Watched < WatchedSystems.size();)
    {
        UAthenaAISystem* System = reinterpret_cast<UAthenaAISystem*>(WatchedSystems[Watched].Get());
        if (!System)
        {
            WatchedSystems.erase(WatchedSystems.begin() + static_cast<ptrdiff_t>(Watched));
            continue;
        }
        ++Watched;
        int32_t Twice = 0;
        const int32_t Unregistered = System->UnregisterPendingKillControllers(Twice);
        int32_t Left = 0;
        for (const AAthenaAIController* Controller : System->GetManagedControllers())
        {
            Left += !Controller || Controller->IsPendingKill() ? 1 : 0;
        }
        if ((Unregistered > 0 || Left > 0) && Logged < 8)
        {
            ++Logged;
            Log::Info(L"Battle Royale AI: as the garbage collection begins, {} pending-kill Athena AI controller entries ({} of them a controller's second) leave {}'s list "
                L"(UAthenaAISystem::UnregisterManagedController), {} controllers listed{}{}",
                Unregistered, Twice, System->GetName().ToStdString(), System->NumManagedControllers(),
                Left > 0 ? std::format(L"; {} entries are still null or pending kill", Left) : std::wstring(), Logged == 8 ? L" (no more of these lines)" : L"");
        }
    }
#endif
}

void UAthenaAISystem::WatchGarbageCollection()
{
#if WITH_SERVER_CODE
    // Epic's own last try, AAthenaAIController::BeginDestroy's unregister, comes after the collector cleared the list's
    // entries for the controllers it is destroying, and the build's unregister and BeginDestroy are intact code (no hook).
    // The collector's own broadcast before it marks what is reachable is where the same unregister still finds them.
    static bool bBound = false;
    static bool bFailed = false;
    if (!Natives::UAthenaAISystem_UnregisterManagedController || bFailed)
    {
        return;
    }
    if (!bBound)
    {
        FSimpleMulticastDelegate* PreGarbageCollect = FCoreUObjectDelegates::GetPreGarbageCollectDelegate();
        const FDelegateHandle Handle = PreGarbageCollect ? PreGarbageCollect->AddLambda([]() { HandlePreGarbageCollect(); }) : FDelegateHandle();
        if (!Handle.IsValid())
        {
            bFailed = true;
            Log::Warning(L"Battle Royale AI: the garbage collection's start can't be bound (FCoreUObjectDelegates::GetPreGarbageCollectDelegate {}), so a pending-kill Athena AI "
                L"controller still in its AI system's list is left there for the collector to clear", PreGarbageCollect ? L"refused the binding" : L"not found");
            return;
        }
        bBound = true;
    }
    for (const FWeakObjectPtr& Watched : WatchedSystems)
    {
        if (Watched.Get() == this)
        {
            return;
        }
    }
    WatchedSystems.emplace_back(this);
    Log::Info(L"Battle Royale AI: {}'s pending-kill Athena AI controllers leave its list as each garbage collection begins (FCoreUObjectDelegates::GetPreGarbageCollectDelegate: "
        L"UAthenaAISystem::UnregisterManagedController){}", GetName().ToStdString(),
        AAthenaAIController::HasManagedTickIndex() ? L"" : L", by their own index only: AAthenaAIController::ManagedTickIndex wasn't found");
#endif
}
