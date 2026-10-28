#pragma once

#include "Components/ActorComponent.h"
#include "Math/Vector.h"
#include "UObject/NameTypes.h"

#include <cstdint>

class APawn;
class USceneComponent;

class UCharacterMovementComponent : public UActorComponent
{
    DefineUnrealClass(UCharacterMovementComponent, Engine)

public:
    DefineUProperty(USceneComponent*, UpdatedComponent)
    DefineUProperty(FVector, Velocity)
    DefineUProperty(APawn*, CharacterOwner)
    DefineUProperty(uint8_t, MovementMode)
    DefineUProperty(uint8_t, CustomMovementMode)
    DefineBitfieldUProperty(bCheatFlying)
    DefineUFunction(SetMovementMode)

    void SetMovementMode(uint8_t NewMovementMode, uint8_t NewCustomMode = 0);

    // Logs the moves whose client location the server disagrees with, on the movement class at ClassPath (#78), and drops
    // the roll the moves carry, which 1.8 fills with something else (#107).
    static void HookServerMove(const wchar_t* ClassPath);

private:
    void ReportClientError(float TimeStamp, const FVector& ClientLoc, uint8_t CompressedMoveFlags, USceneComponent* ClientMovementBase, FName ClientBaseBoneName, uint8_t ClientMovementMode);

    static void ServerMove_ImplementationHK(UCharacterMovementComponent* This, float TimeStamp, const FVector& InAccel, const FVector& ClientLoc, uint8_t CompressedMoveFlags, uint8_t ClientRoll, uint32_t View, USceneComponent* ClientMovementBase, FName ClientBaseBoneName, uint8_t ClientMovementMode);
    static inline void (*ServerMove_ImplementationOG)(UCharacterMovementComponent* This, float TimeStamp, const FVector& InAccel, const FVector& ClientLoc, uint8_t CompressedMoveFlags, uint8_t ClientRoll, uint32_t View, USceneComponent* ClientMovementBase, FName ClientBaseBoneName, uint8_t ClientMovementMode) = nullptr;
};
