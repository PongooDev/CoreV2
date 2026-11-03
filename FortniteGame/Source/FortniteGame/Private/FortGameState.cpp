#include "pch.h"
#include "FortGameState.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UI/FrontEnd/FortMissionGenerator.h"

void AFortGameState::SpawnMissionManager(const FFortMissionManagerRecord& MissionManagerRecord)
{
    if (Natives::AFortGameState_SpawnMissionManager || Finder::ResolveMissionManagerLive(MissionManagerOffset()))
    {
        Natives::AFortGameState_SpawnMissionManager(this, MissionManagerRecord);
    }
}

UFortMissionGenerator* AFortGameStateZone::GetMissionGenerator() const
{
    const TAssetSubclassOf<UFortMissionGenerator>& GeneratorClass = MissionGeneratorClass;
    UClass* LoadedClass = GeneratorClass.LoadSynchronous();
    if (!LoadedClass || !LoadedClass->IsChildOf(UFortMissionGenerator::StaticClass()))
    {
        return nullptr;
    }
    return static_cast<UFortMissionGenerator*>(LoadedClass->GetDefaultObject());
}

// Set by AFortGameStateZone::SetDifficultyIncreaseRewardTier (3.0 0xB7E650, intact), which Mission_BluGloPylon calls on
// each deposit and which refuses a tier above the zone's difficulty row's NumDifficultyIncreases. 3.0's own reader
// left is the mission status analytics (0xCA1140); its EndPrimaryMission sender is stripped (the FName is only made,
// 0xFFF10), so Core's sends it.
int32_t AFortGameStateZone::GetDifficultyIncreaseRewardTier() const
{
    return Member_DifficultyIncreaseRewardTier.IsAvailable() ? DifficultyIncreaseRewardTier : 0;
}

bool AFortGameStateZone::GetPlayerStructures(int32_t& OutTotal, int32_t& OutMax) const
{
    if (!Member_TotalPlayerStructures.IsAvailable() || !Member_MaxPlayerStructures.IsAvailable())
    {
        return false;
    }
    OutTotal = TotalPlayerStructures;
    OutMax = MaxPlayerStructures;
    return true;
}

bool AFortGameStateZone::SetMaxPlayerStructures(int32_t NewMax)
{
    if (!Member_MaxPlayerStructures.IsAvailable())
    {
        return false;
    }
    MaxPlayerStructures = NewMax;
    return true;
}

void AFortGameStateZone::SetGameplayState(EFortGameplayState NewGameplayState)
{
    if (Natives::AFortGameStateZone_SetGameplayStateIndex >= 0)
    {
        CallVirtual<void>(this, Natives::AFortGameStateZone_SetGameplayStateIndex, static_cast<uint8_t>(NewGameplayState));
    }
}

ptrdiff_t AFortGameState::MissionManagerOffset()
{
    return Member_MissionManager.GetOffsetChecked();
}
