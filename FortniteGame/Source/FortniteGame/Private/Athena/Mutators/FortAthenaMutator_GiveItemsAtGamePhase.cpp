#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_GiveItemsAtGamePhase.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"

namespace
{
    // NumberToGive's rows are playlist game data, read at level 0 as the game's other playlist values are.
    constexpr float GiveCurveLevel = 0.0f;
}

void AFortAthenaMutator_GiveItemsAtGamePhase::OnGamePhaseChanged(uint8_t GamePhase)
{
#if WITH_SERVER_CODE
    // As AFortAthenaMutator_GiveItemsAtGamePhaseStep's, from the game state's GamePhaseChanged.
    if (GamePhase != PhaseToGiveItems)
    {
        return;
    }
    const TStructArray<FItemsToGiveAtPhase>& Items = ItemsToGive;
    int32_t Players = 0;
    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        const FItemsToGiveAtPhase& Entry = Items[Index];
        const int32_t Count = static_cast<int32_t>(Entry.NumberToGive.GetValueAtLevel(GiveCurveLevel));
        Players = FMath::Max(Players, GiveItemToAllPlayers(Entry.ItemToDrop, Count));
    }
    Log::Info(L"Battle Royale: {} gives {} items to {} players at phase {}", GetName().ToStdString(), Items.Num(), Players, static_cast<int32_t>(GamePhase));
#endif
}

DEFINE_FUNCTION(AFortAthenaMutator_GiveItemsAtGamePhase::execOnGamePhaseChanged)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_GamePhase);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortAthenaMutator_GiveItemsAtGamePhase*>(Context)->OnGamePhaseChanged(Z_Param_GamePhase);
    P_NATIVE_END;
}
