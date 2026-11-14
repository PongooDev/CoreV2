#include "pch.h"
#include "Athena/AI/AthenaAIController.h"
#include "Athena/AI/AthenaAISystem.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "Log.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"

UFunction* AAthenaAIController::HandleDestroyedFunction = nullptr;

DEFINE_FUNCTION(AAthenaAIController::execHandleDestroyed)
{
    P_GET_OBJECT(AActor, DestroyedActor);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AAthenaAIController*>(Context)->HandleDestroyed(DestroyedActor);
    P_NATIVE_END;
}

void AAthenaAIController::HandleDestroyed(AActor* DestroyedActor)
{
#if WITH_SERVER_CODE
    UAthenaAISystem* System = UAthenaAISystem::Get(GetWorld());
    if (!System || !Natives::UAthenaAISystem_UnregisterManagedController)
    {
        return;
    }
    // The build's unregister tests the controller's own index (-1 once out) and goes by the world's AI system, so a
    // controller its UnPossess took out is left as it is. The list is only looked at for the log.
    const bool bStillManaged = System->IsManagedController(this);
    Natives::UAthenaAISystem_UnregisterManagedController(this);
    static int32_t Logged = 0;
    if (bStillManaged && Logged++ < 8)
    {
        Log::Info(L"Battle Royale AI: {} is destroyed still in {}'s AI controllers (its UnPossess had no pawn to let go): it leaves them now, {} left{}",
            GetName().ToStdString(), System->GetName().ToStdString(), System->NumManagedControllers(), Logged == 8 ? L" (no more of these lines)" : L"");
    }
#endif
}

void AAthenaAIController::BindDestroyed()
{
    if (HandleDestroyedFunction)
    {
        FMulticastScriptDelegate& Delegate = OnDestroyed;
        Delegate.AddUniqueDynamic(this, HandleDestroyedFunction->GetFName());
    }
}

void AAthenaAIController::BindDestroyedForWorld(UWorld* World)
{
#if WITH_SERVER_CODE
    UAthenaAISystem* System = UAthenaAISystem::Get(World);
    if (!System || !StaticClass() || !AActor::HasOnDestroyed() || !UAthenaAISystem::HasAIControllers())
    {
        return;
    }
    static FWeakObjectPtr BoundWorld;
    if (BoundWorld.Get() == reinterpret_cast<UObject*>(World))
    {
        return;
    }
    if (!Finder::ResolveAthenaAIControllerUnregisterLive(UAthenaAISystem::AIControllersOffset()))
    {
        return;
    }
    if (!HandleDestroyedFunction)
    {
        // The game's broadcast calls each bound UFunction through ProcessEvent: Core's native goes on the controller's
        // own class, bound with the controller itself, with OnDestroyed's signature.
        const UProperty* Property = AActor::StaticClass()->FindPropertyByName(FName(L"OnDestroyed"));
        const UFunction* Signature = Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UMulticastDelegateProperty)
            ? static_cast<UFunction*>(static_cast<const UMulticastDelegateProperty*>(Property)->SignatureFunction) : nullptr;
        HandleDestroyedFunction = Signature ? StaticClass()->AddNativeFunction(L"HandleDestroyed", &execHandleDestroyed, Signature) : nullptr;
        if (!HandleDestroyedFunction)
        {
            Log::Warning(L"Battle Royale AI: AAthenaAIController's OnDestroyed can't call Core's HandleDestroyed, so a controller destroyed without a pawn stays in its AI system's list");
            return;
        }
    }
    BoundWorld = FWeakObjectPtr(reinterpret_cast<UObject*>(World));

    int32_t Bound = 0;
    for (AAthenaAIController* Controller : System->GetManagedControllers())
    {
        if (Controller && !Controller->IsPendingKill())
        {
            Controller->BindDestroyed();
            ++Bound;
        }
    }
    const FDelegateHandle Handle = World->AddOnActorSpawnedHandler(FOnActorSpawned::FDelegate::CreateLambda([](AActor* Actor)
    {
        if (AAthenaAIController* Controller = Cast<AAthenaAIController>(reinterpret_cast<UObject*>(Actor)))
        {
            Controller->BindDestroyed();
        }
    }));
    Log::Info(L"Battle Royale AI: {}'s Athena AI controllers leave its list as they are destroyed (AActor::OnDestroyed: UAthenaAISystem::UnregisterManagedController){}, {} already there",
        System->GetName().ToStdString(), Handle.IsValid() ? L"" : L", only the ones already there: the world's OnActorSpawned wasn't found", Bound);
#endif
}
