#include "pch.h"
#include "Building/BuildingGameplayActorSpawnMachine.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Athena/FortPlayerStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "Kismet/GameplayStatics.h"
#include "UObject/WeakObjectPtr.h"

#include <string>
#include <utility>

namespace
{
    // An object pointer member of Object, by its reflected name (Blueprint members Core has no wrapper for).
    UObject* ObjectProperty(UObject* Object, const wchar_t* Name)
    {
        UProperty* Property = Object ? Object->GetClass()->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property && Property->ElementSize == sizeof(UObject*) ? *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(Object) + Property->GetOffset_ForInternal()) : nullptr;
    }

    // The controller of the game state's player whose UniqueId is Id (compared as strings, as AFortMinigame::
    // PlayerStateOf does).
    AFortPlayerControllerAthena* ControllerOf(UWorld* World, const FUniqueNetIdRepl& Id)
    {
        AGameStateBase* GameState = World ? static_cast<AGameStateBase*>(World->GetGameState()) : nullptr;
        if (!GameState || !Id.IsValid())
        {
            return nullptr;
        }
        const std::wstring Wanted = Id.ToString().ToStdString();
        for (APlayerState* Candidate : static_cast<const TArray<APlayerState*>&>(GameState->PlayerArray))
        {
            const FUniqueNetIdRepl* CandidateId = Candidate ? &static_cast<const FUniqueNetIdRepl&>(Candidate->UniqueId) : nullptr;
            if (CandidateId && CandidateId->IsValid() && CandidateId->ToString().ToStdString() == Wanted)
            {
                return Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Candidate->GetOwner()));
            }
        }
        return nullptr;
    }
}

void ABuildingGameplayActorSpawnMachine::StartResurrection()
{
#if WITH_SERVER_CODE
    static bool bLogged = false;
    if (!std::exchange(bLogged, true))
    {
        Log::Info(L"Reboot Van: {} reboots {} player(s)", GetName().ToStdString(), PlayerIdsForResurrection.Num());
    }
    ResurrectPlayer(0);
#endif
}

void ABuildingGameplayActorSpawnMachine::ResurrectPlayer(int32_t Index)
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    TStructArray<FUniqueNetIdRepl>& Ids = PlayerIdsForResurrection;
    if (!World || Index >= Ids.Num())
    {
        OnResurrectionCompleted();
        return;
    }

    AFortPlayerControllerAthena* Controller = ControllerOf(World, Ids[Index]);
    AFortPlayerStateAthena* PlayerState = Controller ? Cast<AFortPlayerStateAthena>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState))) : nullptr;
    // Only a player the interaction marked: a teammate who left, or one already back, stays as they are.
    if (PlayerState && PlayerState->bResurrectingNow)
    {
        // Core's: the engine's ServerRestartPlayer restarts a Spectating controller (SpectateOnDeath put the dead player
        // there) only while bPlayerIsWaiting; RespawnPlayerAfterDeath clears it after the restart.
        if (APlayerController::HasPlayerIsWaiting())
        {
            Controller->bPlayerIsWaiting = true;
        }
        Reflection::FParams Respawn(AFortPlayerControllerAthena::StaticFunction_RespawnPlayerAfterDeath(), true);
        if (Respawn.Set(L"bEnterSkydiving", RebootPlayersInAir()))
        {
            Respawn.Call(Controller);
        }
        APawn* Pawn = Controller->GetPawn();
        if (Pawn)
        {
            // Back among the alive players (RemoveFromAlivePlayers took them out at the death); the game's restart
            // path has no AddToAlivePlayers of its own (8.30 calls it from the starting player's and the team change's
            // slots only).
            AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this);
            if (GameMode && !Controller->bMarkedAlive && Finder::ResolveAddToAlivePlayersLive())
            {
                Natives::AFortGameModeAthena_AddToAlivePlayers(GameMode, Controller);
            }
            // Core's: the rebooted player's count; no code left in the build writes it.
            PlayerState->RebootCounter = PlayerState->RebootCounter + 1;
            OnPlayerPawnResurrected(Pawn);
            Log::Info(L"Reboot Van: {} rebooted {} as {}", GetName().ToStdString(), Controller->GetName().ToStdString(), Pawn->GetName().ToStdString());
        }
        else
        {
            Log::Warning(L"Reboot Van: {} couldn't reboot {} (no pawn after RespawnPlayerAfterDeath)", GetName().ToStdString(), Controller->GetName().ToStdString());
        }
    }

    if (Index + 1 >= Ids.Num())
    {
        OnResurrectionCompleted();
        return;
    }
    const FWeakObjectPtr WeakThis(this);
    const int32_t Next = Index + 1;
    FTimerHandle Handle;
    World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateLambda([WeakThis, Next]()
    {
        if (ABuildingGameplayActorSpawnMachine* SpawnMachine = static_cast<ABuildingGameplayActorSpawnMachine*>(WeakThis.Get()))
        {
            SpawnMachine->ResurrectPlayer(Next);
        }
    }), ResurrectionNextPlayerDelay.GetValueAtLevel(0.0f), false);
#endif
}

void ABuildingGameplayActorSpawnMachine::OnPlayerPawnResurrected(APawn* PlayerPawn)
{
    // A Blueprint event: the Blueprint's own function (BGA_Athena_SCMachine_C's), not the native one.
    UFunction* Function = FindFunction(FName(L"OnPlayerPawnResurrected", FNAME_Find));
    Reflection::FParams Params(Function, true);
    if (Params.Set(L"PlayerPawn", reinterpret_cast<UObject*>(PlayerPawn)))
    {
        Params.Call(this);
    }
}

void ABuildingGameplayActorSpawnMachine::OnResurrectionCompleted()
{
    UFunction* Function = FindFunction(FName(L"OnResurrectionCompleted", FNAME_Find));
    Reflection::FParams Params(Function, true);
    Params.Call(this);
}

bool ABuildingGameplayActorSpawnMachine::RebootPlayersInAir()
{
    // The game's Athena.bEnterSkyDivingOnReboot (8.30 0xFF0750).
    Reflection::FParams Params(FindFunction(FName(L"RebootPlayersInAir", FNAME_Find)), true);
    return Params.Call(this) && Params.Take<bool>(L"ReturnValue");
}

void ABuildingGameplayActorSpawnMachine::SetUpResurrectLocations(UWorld* World)
{
#if WITH_SERVER_CODE
    UClass* Class = StaticClass();
    UClass* PlayerStartClass = Class ? FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortPlayerStart") : nullptr;
    if (!World || !PlayerStartClass || !HasResurrection())
    {
        return;
    }
    const TArray<AActor*> Vans = UGameplayStatics::GetAllActorsOfClass(World, Class);
    int32_t Set = 0;
    for (AActor* Actor : Vans)
    {
        ABuildingGameplayActorSpawnMachine* SpawnMachine = static_cast<ABuildingGameplayActorSpawnMachine*>(Actor);
        // The Blueprint's ResurrectionLocation, a UChildActorComponent with a FortPlayerStart child.
        AActor* Start = reinterpret_cast<AActor*>(ObjectProperty(ObjectProperty(reinterpret_cast<UObject*>(SpawnMachine), L"ResurrectionLocation"), L"ChildActor"));
        if (SpawnMachine && !SpawnMachine->ResurrectLocation && Start && reinterpret_cast<UObject*>(Start)->IsA(PlayerStartClass))
        {
            SpawnMachine->ResurrectLocation = Start;
            ++Set;
        }
    }
    if (Vans.Num() > 0)
    {
        Log::Info(L"Reboot Van: {} of the world's {} vans reboot at their ResurrectionLocation", Set, Vans.Num());
    }
#endif
}

void ABuildingGameplayActorSpawnMachine::HookNatives()
{
#if WITH_SERVER_CODE
    UClass* Class = StaticClass();
    UObject* SpawnMachineDefault = Class ? Class->GetDefaultObject(false) : nullptr;
    UClass* InteractInterface = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInteractInterface");
    UProperty* StartDelay = Class ? Class->FindPropertyByName(FName(L"ResurrectionStartDelay", FNAME_Find)) : nullptr;
    if (!SpawnMachineDefault || !InteractInterface || !StartDelay || !HasResurrection() || !AFortPlayerStateAthena::HasResurrection()
        || !AFortPlayerControllerAthena::FindStaticFunction_RespawnPlayerAfterDeath() || !FTimerManager::ResolveNatives())
    {
        return;
    }
    uint8_t* Bind = Finder::SpawnMachineResurrectionTimerBind(SpawnMachineDefault, InteractInterface, StartDelay->GetOffset_ForInternal());
    if (!Bind)
    {
        Log::Error(L"Reboot Van: the van's reboot timer wasn't found, so nobody is rebooted");
        return;
    }
    Hooks::RedirectAddress(L"ABuildingGameplayActorSpawnMachine::StartResurrection (ServerOnAttemptInteract's timer)", Bind, reinterpret_cast<void*>(&StartResurrectionHK));
    Log::Info(L"Finder: ABuildingGameplayActorSpawnMachine's reboot is stripped, restored (StartResurrection on the interaction's timer)");
#endif
}

void ABuildingGameplayActorSpawnMachine::StartResurrectionHK(ABuildingGameplayActorSpawnMachine* This)
{
    This->StartResurrection();
}
