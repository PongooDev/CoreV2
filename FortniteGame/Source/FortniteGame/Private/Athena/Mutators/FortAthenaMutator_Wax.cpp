#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_Wax.h"
#include "Athena/FortGameStateAthena.h"
#include "Engine/World.h"
#include "Templates/Casts.h"

AFortAthenaMutator_Wax* AFortAthenaMutator_Wax::GetCurrent(UWorld* World)
{
    AFortGameStateAthena* GameState = World && StaticClass() ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const TArray<UObject*>* MutatorList = GameState ? GameState->GetGameplayMutatorList() : nullptr;
    if (!MutatorList)
    {
        return nullptr;
    }
    const TArray<UObject*>& Mutators = *MutatorList;
    for (int32_t Index = 0; Index < Mutators.Num(); ++Index)
    {
        if (AFortAthenaMutator_Wax* Wax = Cast<AFortAthenaMutator_Wax>(Mutators[Index]))
        {
            return Wax;
        }
    }
    return nullptr;
}
