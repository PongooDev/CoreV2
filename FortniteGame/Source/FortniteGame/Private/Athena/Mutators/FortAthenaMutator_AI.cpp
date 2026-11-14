#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_AI.h"
#include "Athena/FortGameStateAthena.h"
#include "Log.h"
#include "Pawns/FortMovementComp_Character.h"
#include "Templates/Casts.h"

bool AFortAthenaMutator_AI::FortGameStateAthena_AddManagedCharacterMovementComponent(AFortGameStateAthena* GameState, UFortMovementComp_Character* MovementComponent)
{
#if WITH_SERVER_CODE
    // The game state asks only while the mutator is active. A Cube Monster's movement goes into the mutator's batch, with
    // its place kept in ManagedTickIndex as AFortGameState's list keeps it; any other movement stays the game state's.
    UFortMovementComp_AIChar* AIMovement = Cast<UFortMovementComp_AIChar>(MovementComponent);
    if (!AIMovement || !AIMovement->bDeimos)
    {
        return false;
    }
    TArray<UFortMovementComp_AIChar*>& Components = DeimosMoveComponents;
    if (!Components.Contains(AIMovement))
    {
        MovementComponent->ManagedTickIndex = Components.Add(AIMovement);
        static bool bLogged = false;
        if (!bLogged)
        {
            bLogged = true;
            Log::Info(L"AFortAthenaMutator_AI: {} takes the Cube Monsters' movement, the first {} (its batch moves {})", GetName().ToStdString(),
                reinterpret_cast<UObject*>(MovementComponent)->GetName().ToStdString(), Components.Num());
        }
    }
    return true;
#else
    return false;
#endif
}

bool AFortAthenaMutator_AI::FortGameStateAthena_RemoveManagedCharacterMovementComponent(AFortGameStateAthena* GameState, UFortMovementComp_Character* MovementComponent)
{
#if WITH_SERVER_CODE
    // As AFortGameState::RemoveManagedCharacterMovementComponent (6.22 0xF43930): RemoveAtSwap at the component's place,
    // the one moved into it gets that place. A component the mutator never took (added before it was active) is the
    // game state's to remove.
    UFortMovementComp_AIChar* AIMovement = Cast<UFortMovementComp_AIChar>(MovementComponent);
    if (!AIMovement)
    {
        return false;
    }
    TArray<UFortMovementComp_AIChar*>& Components = DeimosMoveComponents;
    int32_t Index = MovementComponent->ManagedTickIndex;
    if (!Components.IsValidIndex(Index) || Components[Index] != AIMovement)
    {
        Index = -1;
        for (int32_t At = 0; At < Components.Num() && Index < 0; ++At)
        {
            Index = Components[At] == AIMovement ? At : -1;
        }
    }
    if (Index < 0)
    {
        return false;
    }
    Components.RemoveAtSwap(Index);
    if (Components.IsValidIndex(Index) && Components[Index])
    {
        static_cast<UFortMovementComp_Character*>(Components[Index])->ManagedTickIndex = Index;
    }
    MovementComponent->ManagedTickIndex = -1;
    return true;
#else
    return false;
#endif
}

bool AFortAthenaMutator_AI::AddManagedCharacterMovementComponentHK(AFortAthenaMutator_AI* This, AFortGameStateAthena* GameState, UFortMovementComp_Character* MovementComponent)
{
    return This && This->FortGameStateAthena_AddManagedCharacterMovementComponent(GameState, MovementComponent);
}

bool AFortAthenaMutator_AI::RemoveManagedCharacterMovementComponentHK(AFortAthenaMutator_AI* This, AFortGameStateAthena* GameState, UFortMovementComp_Character* MovementComponent)
{
    return This && This->FortGameStateAthena_RemoveManagedCharacterMovementComponent(GameState, MovementComponent);
}
