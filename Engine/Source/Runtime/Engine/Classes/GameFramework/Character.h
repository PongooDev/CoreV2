#pragma once

#include "GameFramework/Pawn.h"

#include <cstdint>

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

public:
    DefineUProperty(UCharacterMovementComponent*, CharacterMovement)

    // The body of ClientCheatWalk, ClientCheatFly or ClientCheatGhost.
    void ApplyCheatMovement(ECheatMovement Movement);

    // Server and client: the Walk, Fly and Ghost cheats move the pawn (#100).
    static void HookCheatMovement();

private:
    static void ClientCheatWalk_ImplementationHK(ACharacter* This);
    static void ClientCheatFly_ImplementationHK(ACharacter* This);
    static void ClientCheatGhost_ImplementationHK(ACharacter* This);
};
