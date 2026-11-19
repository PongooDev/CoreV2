#pragma once

#include "GameFramework/Actor.h"

class APawn;
class APlayerState;

class AController : public AActor
{
    DefineUnrealClass(AController, Engine)
    DefineUFunction(Possess)
    DefineUFunction(UnPossess)
    DefineUFunction(SetControlRotation)
    DefineUFunction(ClientSetRotation)

public:
    DefineUProperty(APawn*, Pawn)
    DefineUProperty(APlayerState*, PlayerState)
    DefineUProperty(FRotator, ControlRotation)

    APawn* GetPawn() const { return Pawn; }
    AActor* GetViewTarget() const;
    // Server: lets go of the pawn (AController::UnPossess, through its UFunction).
    void UnPossess();
    // Server: takes InPawn (AController::Possess, through its UFunction).
    void Possess(APawn* InPawn);
    // The engine's, through their UFunctions: the server's control rotation, and the client RPC that sets the owning
    // client's (AGameModeBase::FinishRestartPlayer sends a restarted player ClientSetRotation(Pawn's rotation, true)).
    // False when the build lacks either.
    bool SetControlRotation(const FRotator& NewRotation);
    bool ClientSetRotation(const FRotator& NewRotation, bool bResetCamera = false);
};
