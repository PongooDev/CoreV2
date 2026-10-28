#include "pch.h"
#include "GameFramework/Character.h"
#include "Finder.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Hooks.h"
#include "Log.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <tuple>

namespace
{
    // EMovementMode.
    constexpr uint8_t MoveFalling = 3;
    constexpr uint8_t MoveFlying = 5;

    // What shipping builds leave of a function whose body is compiled out: "ret 0", shared by all of them.
    bool IsEmptyFunction(const void* Function)
    {
        const uint8_t* const Code = static_cast<const uint8_t*>(Function);
        return Code && (Code[0] == 0xC3 || (Code[0] == 0xC2 && Code[1] == 0x00 && Code[2] == 0x00));
    }
}

void ACharacter::ApplyCheatMovement(ECheatMovement Movement)
{
    // The engine's ClientCheatWalk/Fly/Ghost_Implementation: collision is off only for Ghost, Fly and Ghost fly and Walk
    // falls back to the ground. Walk only lands a pawn that flies, so it doesn't end a skydive (a custom mode) and does
    // nothing when it runs a second time on the client (the RPC, then bCheatFly's notify).
    SetActorEnableCollision(Movement != ECheatMovement::Ghost);
    UCharacterMovementComponent* const MovementComponent = CharacterMovement;
    if (!MovementComponent)
    {
        return;
    }
    MovementComponent->bCheatFlying = Movement != ECheatMovement::Walk;
    if (Movement != ECheatMovement::Walk)
    {
        MovementComponent->SetMovementMode(MoveFlying);
    }
    else if (MovementComponent->MovementMode == MoveFlying)
    {
        MovementComponent->SetMovementMode(MoveFalling);
    }
}

void ACharacter::HookCheatMovement()
{
    // UCheatManager::Fly, Walk and Ghost (1.7.2 0x1FD9100, 0x1FEC820, 0x1FDD7F0; UFortCheatManager's then set the
    // controller's bCheatFly/bCheatGhost) send the pawn's ClientCheatFly/Walk/Ghost RPC and, on the server, also run its
    // _Implementation for a pawn another machine controls. Shipping builds compile those bodies out: ACharacter's slots
    // and every subclass's hold the shared "ret 0" (1.7.2 0x824760, 1.8 0x1A68A80), so "cheat fly" answered "You feel
    // much lighter" and nothing moved on either side (#100). Both sides now run the engine's body, so the server's
    // movement agrees with the client's and doesn't correct it back.
    UObject* const CharacterDefault = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** const CharacterVTable = CharacterDefault ? *reinterpret_cast<void***>(CharacterDefault) : nullptr;
    for (const auto& [Function, Implementation, Name] : {
             std::tuple{ StaticFunction_ClientCheatWalk(), reinterpret_cast<void*>(&ClientCheatWalk_ImplementationHK), L"ACharacter::ClientCheatWalk_Implementation" },
             std::tuple{ StaticFunction_ClientCheatFly(), reinterpret_cast<void*>(&ClientCheatFly_ImplementationHK), L"ACharacter::ClientCheatFly_Implementation" },
             std::tuple{ StaticFunction_ClientCheatGhost(), reinterpret_cast<void*>(&ClientCheatGhost_ImplementationHK), L"ACharacter::ClientCheatGhost_Implementation" },
         })
    {
        const int32_t Index = Finder::ImplementationIndex(Function);
        void* const Original = CharacterVTable && Index >= 0 ? CharacterVTable[Index] : nullptr;
        if (!Original)
        {
            Log::Error(L"Finder: {} slot not found", Name);
            continue;
        }
        if (!IsEmptyFunction(Original))
        {
            Log::Info(L"{} has a body in this build, left as it is", Name);
            continue;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Original)
            {
                Hooks::SwapVTable(Name, VTable, Index, Implementation);
            }
        }
    }
}

void ACharacter::ClientCheatWalk_ImplementationHK(ACharacter* This)
{
    This->ApplyCheatMovement(ECheatMovement::Walk);
    if (OnCheatMovement)
    {
        OnCheatMovement(This, ECheatMovement::Walk);
    }
}

void ACharacter::ClientCheatFly_ImplementationHK(ACharacter* This)
{
    This->ApplyCheatMovement(ECheatMovement::Fly);
    if (OnCheatMovement)
    {
        OnCheatMovement(This, ECheatMovement::Fly);
    }
}

void ACharacter::ClientCheatGhost_ImplementationHK(ACharacter* This)
{
    This->ApplyCheatMovement(ECheatMovement::Ghost);
    if (OnCheatMovement)
    {
        OnCheatMovement(This, ECheatMovement::Ghost);
    }
}
