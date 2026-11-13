#pragma once

#include "GameFramework/Pawn.h"

#include <cstdint>

class UCapsuleComponent;
class UCharacterMovementComponent;

enum class ECheatMovement : uint8_t
{
    Walk,
    Fly,
    Ghost,
};

class ACharacter : public APawn
{
    DefineUnrealClass(ACharacter, Engine)
    DefineUFunction(ClientCheatWalk)
    DefineUFunction(ClientCheatFly)
    DefineUFunction(ClientCheatGhost)
    DefineUFunction(Jump)
    DefineUFunction(PlayAnimMontage)
    DefineUFunction(StopAnimMontage)

public:
    DefineUProperty(UCharacterMovementComponent*, CharacterMovement)
    DefineUProperty(UCapsuleComponent*, CapsuleComponent)
    // How many jumps the character may make before landing (replicated; 1 by default).
    DefineUProperty(int32_t, JumpMaxCount)

    // ACharacter::GetDefaultHalfHeight: the half height of the class default's capsule, with its scale (what a spawn
    // raises the pawn's location by); 0 without one.
    float GetDefaultHalfHeight() const;

    // The body of ClientCheatWalk, ClientCheatFly or ClientCheatGhost.
    void ApplyCheatMovement(ECheatMovement Movement);
    // The character jumps on its next movement update.
    void Jump();
    // ACharacter::PlayAnimMontage (UAnimMontage* AnimMontage, float InPlayRate, FName StartSectionName): the montage's
    // length, 0 when it didn't play. AFortPawn's override plays it through its ability system
    // (UAbilitySystemComponent::PlayMontage), which replicates it to the other players.
    float PlayAnimMontage(UObject* AnimMontage, float InPlayRate = 1.0f, FName StartSectionName = FName());
    // ACharacter::StopAnimMontage: null stops the current one (AFortPawn's: the ability system's CurrentMontageStop).
    void StopAnimMontage(UObject* AnimMontage = nullptr);

    // Server and client: the Walk, Fly and Ghost cheats move the pawn (#100).
    static void HookCheatMovement();

    // Runs after ClientCheatWalk, ClientCheatFly or ClientCheatGhost on the machine it runs on: the server for a pawn
    // another machine controls, and the pawn's own client. FortniteGame sets it for the speed "cheat fly 3000" asks for
    // (#129).
    static inline void (*OnCheatMovement)(ACharacter* Character, ECheatMovement Movement) = nullptr;

private:
    static void ClientCheatWalk_ImplementationHK(ACharacter* This);
    static void ClientCheatFly_ImplementationHK(ACharacter* This);
    static void ClientCheatGhost_ImplementationHK(ACharacter* This);
};
