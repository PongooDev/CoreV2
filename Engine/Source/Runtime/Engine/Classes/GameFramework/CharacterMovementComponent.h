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

    // Logs the moves whose client location the server disagrees with, on the movement class at ClassPath (#78).
    static void HookServerMove(const wchar_t* ClassPath);

private:
    void ReportClientError(float TimeStamp, const FVector& ClientLoc, uint8_t CompressedMoveFlags, USceneComponent* ClientMovementBase, FName ClientBaseBoneName, uint8_t ClientMovementMode);

    static void ServerMove_ImplementationHK(UCharacterMovementComponent* This, float TimeStamp, const FVector& InAccel, const FVector& ClientLoc, uint8_t CompressedMoveFlags, uint8_t ClientRoll, uint32_t View, USceneComponent* ClientMovementBase, FName ClientBaseBoneName, uint8_t ClientMovementMode);
    static inline void (*ServerMove_ImplementationOG)(UCharacterMovementComponent* This, float TimeStamp, const FVector& InAccel, const FVector& ClientLoc, uint8_t CompressedMoveFlags, uint8_t ClientRoll, uint32_t View, USceneComponent* ClientMovementBase, FName ClientBaseBoneName, uint8_t ClientMovementMode) = nullptr;
};
