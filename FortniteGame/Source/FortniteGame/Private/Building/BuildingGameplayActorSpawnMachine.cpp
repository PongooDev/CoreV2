#include "pch.h"
#include "Building/BuildingGameplayActorSpawnMachine.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
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

AFortGameStateAthena* ABuildingGameplayActorSpawnMachine::GetAthenaGameState() const
{
    UWorld* World = GetWorld();
    return World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
}

void ABuildingGameplayActorSpawnMachine::SetUpSpawnMachines(UWorld* World)
{
#if WITH_SERVER_CODE
    UClass* Class = StaticClass();
    UClass* PlayerStartClass = Class ? FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortPlayerStart") : nullptr;
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState || !PlayerStartClass || !HasResurrection())
    {
        return;
    }
    // The match has Reboot: the game state's IsResurrectionEnabled without a pawn (8.30 0xF4ECF0: the
    // Athena.bResurrectionFunctionalityEnabled console variable and the playlist's ResurrectionEnabledRow).
    bool bRebootEnabled = false;
    Reflection::FParams Enabled(GameState->FindFunction(FName(L"IsResurrectionEnabled", FNAME_Find)), true);
    if (Enabled.Set(L"PlayerPawn", static_cast<UObject*>(nullptr)) && Enabled.Call(reinterpret_cast<UObject*>(GameState)))
    {
        bRebootEnabled = Enabled.Take<bool>(L"ReturnValue");
    }
    const bool bRepData = bRebootEnabled && AFortGameStateAthena::HasSpawnMachineRepData() && Member_SpawnMachineState.IsAvailable() && Member_IndexIntoGameState.IsAvailable();
    FSpawnMachineRepDataArray& RepData = GameState->SpawnMachineRepData;
    TStructArray<FSpawnMachineRepData>& Items = RepData.SpawnMachineRepDataArray;
    // Once a match: only this fills the array. IndexIntoGameState starts at 0 (the van's constructor, 0x1046BF0, leaves it),
    // so an empty array is what says no van has an item yet.
    const bool bList = bRepData && Items.Num() == 0;

    const TArray<AActor*> Vans = UGameplayStatics::GetAllActorsOfClass(World, Class);
    int32_t Set = 0;
    int32_t Listed = 0;
    for (AActor* Actor : Vans)
    {
        ABuildingGameplayActorSpawnMachine* SpawnMachine = static_cast<ABuildingGameplayActorSpawnMachine*>(Actor);
        if (!SpawnMachine || reinterpret_cast<UObject*>(SpawnMachine)->IsPendingKill())
        {
            continue;
        }
        // The Blueprint's ResurrectionLocation, a UChildActorComponent with a FortPlayerStart child.
        AActor* Start = reinterpret_cast<AActor*>(ObjectProperty(ObjectProperty(reinterpret_cast<UObject*>(SpawnMachine), L"ResurrectionLocation"), L"ChildActor"));
        if (!SpawnMachine->ResurrectLocation && Start && reinterpret_cast<UObject*>(Start)->IsA(PlayerStartClass))
        {
            SpawnMachine->ResurrectLocation = Start;
            ++Set;
        }
        if (bList)
        {
            const int32_t Index = Items.Num();
            FSpawnMachineRepData& Item = Items.AddDefaulted();
            Item.Location = SpawnMachine->GetTransform().GetLocation();
            Item.SpawnMachineState = SpawnMachine->SpawnMachineState;
            RepData.MarkItemDirty(Item);
            SpawnMachine->IndexIntoGameState = Index;
            ++Listed;
        }
    }
    if (Listed > 0)
    {
        GameState->ForceNetUpdate();
    }
    if (Vans.Num() > 0)
    {
        Log::Info(L"Reboot Van: {} of the world's {} vans reboot at their ResurrectionLocation; {} on the game state's SpawnMachineRepData for the map (the match {} Reboot)", Set,
            Vans.Num(), Listed, bRebootEnabled ? L"has" : L"has no");
    }
#endif
}

void ABuildingGameplayActorSpawnMachine::UpdateSpawnMachineRepData()
{
#if WITH_SERVER_CODE
    AFortGameStateAthena* GameState = Member_IndexIntoGameState.IsAvailable() ? GetAthenaGameState() : nullptr;
    const int32_t Index = GameState ? static_cast<int32_t>(IndexIntoGameState) : -1;
    FSpawnMachineRepDataArray* RepData = Index >= 0 ? &static_cast<FSpawnMachineRepDataArray&>(GameState->SpawnMachineRepData) : nullptr;
    TStructArray<FSpawnMachineRepData>* Items = RepData ? &static_cast<TStructArray<FSpawnMachineRepData>&>(RepData->SpawnMachineRepDataArray) : nullptr;
    if (!Items || Index >= Items->Num())
    {
        return;
    }
    FSpawnMachineRepData& Item = (*Items)[Index];
    const uint8_t State = SpawnMachineState;
    Item.SpawnMachineState = State;
    static const uint8_t OnCooldown = ESpawnMachineState::GetOnCooldown();
    if (State == OnCooldown)
    {
        float CooldownLength = 0.0f;
        Reflection::FParams Length(FindFunction(FName(L"GetCooldownLength", FNAME_Find)), true);
        if (Length.Call(this))
        {
            CooldownLength = Length.Take<float>(L"ReturnValue");
        }
        UWorld* World = GetWorld();
        const float Now = World ? World->GetTimeSeconds() : 0.0f;
        Item.SpawnMachineCooldownStartTime = Now;
        Item.SpawnMachineCooldownEndTime = Now + CooldownLength;
    }
    RepData->MarkItemDirty(Item);
    GameState->ForceNetUpdate();
#endif
}

DEFINE_FUNCTION(ABuildingGameplayActorSpawnMachine::execSetSpawnMachineState)
{
    SetSpawnMachineStateNative(Context, TheStack, RESULT_PARAM);
#if WITH_SERVER_CODE
    static_cast<ABuildingGameplayActorSpawnMachine*>(Context)->UpdateSpawnMachineRepData();
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

    UFunction* SetState = AFortGameStateAthena::HasSpawnMachineRepData() && Member_IndexIntoGameState.IsAvailable() ? FindStaticFunction_SetSpawnMachineState() : nullptr;
    if (SetState && SetState->GetNativeFunc() && SetState->GetNativeFunc() != &execSetSpawnMachineState)
    {
        SetSpawnMachineStateNative = SetState->GetNativeFunc();
        SetState->SetNativeFunc(&execSetSpawnMachineState);
        Log::Info(L"Reboot Van: SetSpawnMachineState's native keeps the van's SpawnMachineRepData item in step (its server update is stripped)");
    }
#endif
}

void ABuildingGameplayActorSpawnMachine::StartResurrectionHK(ABuildingGameplayActorSpawnMachine* This)
{
    This->StartResurrection();
}
