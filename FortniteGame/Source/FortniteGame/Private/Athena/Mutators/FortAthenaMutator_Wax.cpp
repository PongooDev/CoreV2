#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_Wax.h"
#include "Athena/FortGameStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
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

void AFortAthenaMutator_Wax::FortGameStateAthena_OnClientPawnLoaded(AController* Controller)
{
#if WITH_SERVER_CODE
    if (Controller && Natives::AFortAthenaMutator_Wax_OnClientPawnLoadedIndex >= 0)
    {
        CallVirtual<uint8_t>(this, Natives::AFortAthenaMutator_Wax_OnClientPawnLoadedIndex, Controller);
    }
#endif
}

void AFortAthenaMutator_Wax::HookNatives()
{
#if WITH_SERVER_CODE
    if (!StaticClass())
    {
        return;
    }
    Finder::ResolveWaxOnClientPawnLoadedLive();
#endif
}
