#include "pch.h"
#include "Athena/Mutators/FortAthenaMutator_GiveItemsAtGamePhaseStep.h"
#include "Athena/Mutators/FortAthenaMutator_GiveItemsAtGamePhase.h"
#include "Finder.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "UObject/Class.h"

namespace
{
    // NumberToGive's rows are playlist game data (AthenaCompositeGD_GG's GG.InitialWeaponGrant.*), read at level 0 as the
    // game's other playlist values are; the glider's and the grapplers' have no curve (Value 1).
    constexpr float GiveCurveLevel = 0.0f;
}

void AFortAthenaMutator_GiveItemsAtGamePhaseStep::OnGamePhaseStepChanged(uint8_t GamePhaseStep)
{
#if WITH_SERVER_CODE
    // BeginPlay binds this only on the authority and only while the step is below PhaseToGiveItems, and the game state
    // broadcasts each step once, as it moves on.
    if (GamePhaseStep != PhaseToGiveItems)
    {
        return;
    }
    const TStructArray<FItemsToGive>& Items = ItemsToGive;
    int32_t Players = 0;
    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        const FItemsToGive& Entry = Items[Index];
        const int32_t Count = static_cast<int32_t>(Entry.NumberToGive.GetValueAtLevel(GiveCurveLevel));
        Players = FMath::Max(Players, GiveItemToAllPlayers(Entry.ItemToDrop, Count));
    }
    Log::Info(L"Battle Royale: {} gives {} items to {} players at step {}", GetName().ToStdString(), Items.Num(), Players, static_cast<int32_t>(GamePhaseStep));
#endif
}

DEFINE_FUNCTION(AFortAthenaMutator_GiveItemsAtGamePhaseStep::execOnGamePhaseStepChanged)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_GamePhaseStep);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortAthenaMutator_GiveItemsAtGamePhaseStep*>(Context)->OnGamePhaseStepChanged(Z_Param_GamePhaseStep);
    P_NATIVE_END;
}

void AFortAthenaMutator_GiveItemsAtGamePhaseStep::HookNatives()
{
#if WITH_SERVER_CODE
    // Both classes are 8.00's. Two natives with bodies of their own never share an exec: 8.30, 8.40, 8.50 and 8.51 give
    // both UFunctions the one exec, which steps over the enum and tail-calls an empty function. The exec isn't decoded
    // (on a packed exe its code may have moved).
    UFunction* StepChanged = StaticClass() ? FindStaticFunction_OnGamePhaseStepChanged() : nullptr;
    UFunction* PhaseChanged = AFortAthenaMutator_GiveItemsAtGamePhase::StaticClass() ? AFortAthenaMutator_GiveItemsAtGamePhase::FindStaticFunction_OnGamePhaseChanged() : nullptr;
    if (!StepChanged || !PhaseChanged)
    {
        return;
    }
    const FNativeFuncPtr Exec = StepChanged->GetNativeFunc();
    if (!Exec || Exec != PhaseChanged->GetNativeFunc())
    {
        Log::Info(L"Finder: the give-items mutators' OnGamePhaseStepChanged and OnGamePhaseChanged have the build's own bodies; left as they are");
        return;
    }
    if (!HasItemsToGive() || !AFortAthenaMutator_GiveItemsAtGamePhase::HasItemsToGive())
    {
        Log::Error(L"Finder: the give-items mutators' natives are stripped but their members aren't all reflected; not restored");
        return;
    }
    StepChanged->SetNativeFunc(&execOnGamePhaseStepChanged);
    PhaseChanged->SetNativeFunc(&AFortAthenaMutator_GiveItemsAtGamePhase::execOnGamePhaseChanged);
    bNativesRestored = true;
    Log::Info(L"Finder: AFortAthenaMutator_GiveItemsAtGamePhaseStep::OnGamePhaseStepChanged and AFortAthenaMutator_GiveItemsAtGamePhase::OnGamePhaseChanged are stripped (shared exec 0x{:X}), restored (natives)",
        reinterpret_cast<uintptr_t>(Exec) - Finder::ImageBase());
#endif
}
