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
    // How many jumps the character may make before landing (replicated; 1 by default).
    DefineUProperty(int32_t, JumpMaxCount)

    // The body of ClientCheatWalk, ClientCheatFly or ClientCheatGhost.
    void ApplyCheatMovement(ECheatMovement Movement);

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
