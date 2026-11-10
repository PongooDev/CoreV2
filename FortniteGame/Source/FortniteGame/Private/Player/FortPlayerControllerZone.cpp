#include "pch.h"
#include "Player/FortPlayerControllerZone.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "Hooks.h"
#include "Log.h"
#include "Missions/FortMissionLibrary.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState || GameState->GameplayState != EFortGameplayState::GetEndOfZone() || SetupTeleportOutCameraGameState.Get() == GameState)
    {
        return;
    }

    SetupTeleportOutCameraGameState = FWeakObjectPtr(GameState);
    UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"SetupTeleportOutCamera", FNAME_Find), nullptr);
#endif
}

void AFortPlayerControllerZone::HookPossessionVTables()
{
#if WITH_SERVER_CODE
    // A client that takes a pawn (ClientRestart) answers with ServerAcknowledgePossession, and the server keeps the pawn
    // as the controller's AcknowledgedPawn. The server moves a pawn only for a client that has acknowledged it:
    // UCharacterMovementComponent::ServerMove_Implementation (CL-4204761 0x22A6890) asks
    // APlayerController::NotifyServerReceivedClientData (0x2608D30, slot 0x191), which is false while AcknowledgedPawn
    // isn't the pawn, and then drops the move. 5.0 overrides _Implementation in this class (slot 0x104, the exec
    // 0x28C2DD0's last call) and strips it to the shared empty function, which Battle Royale's controller inherits;
    // 4.5's classes (CL-4166199) all kept APlayerController's (0x23A51C0). No 5.0 pawn was ever acknowledged, so the
    // server never moved one: it hung where it spawned, and after the bus it kept skydiving where it left the bus
    // (#345, #346, #347, #348). Its abilities were refused (Jump and Sprint need a pawn on the ground, interacting one
    // that isn't skydiving), auto pickup saw a pawn that never moved, and the glider couldn't open.
    UFunction* const Function = APlayerController::FindStaticFunction_ServerAcknowledgePossession();
    const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
    UClass* const SuperClass = StaticClass()->GetSuperClass();
    UObject* const SuperDefault = SuperClass ? SuperClass->GetDefaultObject(false) : nullptr;
    void* const Super = SuperDefault && Index >= 0 ? (*reinterpret_cast<void***>(SuperDefault))[Index] : nullptr;
    if (!Super || Finder::IsStubFunction(reinterpret_cast<uintptr_t>(Super)))
    {
        Log::Error(L"Finder: APlayerController::ServerAcknowledgePossession_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            ServerAcknowledgePossession_ImplementationSuper = reinterpret_cast<decltype(ServerAcknowledgePossession_ImplementationSuper)>(Super);
            Hooks::SwapVTable(L"AFortPlayerControllerZone::ServerAcknowledgePossession_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerAcknowledgePossession_ImplementationHK));
        }
    }
#endif
}

void AFortPlayerControllerZone::ServerAcknowledgePossession_ImplementationHK(AFortPlayerControllerZone* This, APawn* P)
{
    This->ServerAcknowledgePossession_Implementation(P);
}

void AFortPlayerControllerZone::ServerAcknowledgePossession_Implementation(APawn* P)
{
    // Super: APlayerController's, which sets AcknowledgedPawn.
    ServerAcknowledgePossession_ImplementationSuper(this, P);
}

void AFortPlayerControllerZone::HookVTables()
{
    HookPossessionVTables();

    const int32_t Index = Finder::ImplementationIndex(StaticFunction_ServerTriggerSetupTeleportCameraEvent());
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerTriggerSetupTeleportCameraEvent_ImplementationHK));
        }
    }
}

void AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerZone* This)
{
    This->ServerTriggerSetupTeleportCameraEvent_Implementation();
}
