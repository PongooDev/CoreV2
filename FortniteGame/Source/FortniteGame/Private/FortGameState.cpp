#include "pch.h"
#include "FortGameState.h"
#include "Finder.h"
#include "Natives.h"
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

ptrdiff_t AFortGameState::MissionManagerOffset()
{
    return Member_MissionManager.GetOffsetChecked();
}
