#include "pch.h"
#include "FortSpecialActorReplicationInfo.h"
#include "Athena/FortAthenaExitCraft.h"
#include "Athena/FortAthenaExitCraftSpawner.h"
#include "Athena/FortGameStateAthena.h"
#include "Finder.h"
#include "Misc/CoreMiscDefines.h"
#include "Natives.h"

FSpecialActorInitData::FSpecialActorInitData()
{
    Natives::FSpecialActorInitData_FSpecialActorInitData(this);
}

FSpecialActorInitData::~FSpecialActorInitData()
{
    Natives::FSpecialActorInitData_Destructor(this);
}

int32_t AFortSpecialActorReplicationInfo::AddActorToReplicationList(const FSpecialActorInitData& InitData)
{
    return Natives::AFortSpecialActorReplicationInfo_AddActorToReplicationList ? Natives::AFortSpecialActorReplicationInfo_AddActorToReplicationList(this, &InitData) : INDEX_NONE;
}

void AFortSpecialActorReplicationInfo::RemoveActorFromReplicationList(FName UniqueID)
{
    if (Natives::AFortSpecialActorReplicationInfo_RemoveActorFromReplicationList)
    {
        Natives::AFortSpecialActorReplicationInfo_RemoveActorFromReplicationList(this, UniqueID);
    }
}

bool AFortSpecialActorReplicationInfo::ResolveNatives()
{
    // The exit crafts' ids are where the natives are found from; the list and the crafts are both new in 5.40.
    const ptrdiff_t SpawnerSpecialActorID = AFortAthenaExitCraftSpawner::SpawnerSpecialActorIDOffset();
    const ptrdiff_t CraftSpecialActorID = AFortAthenaExitCraft::CraftSpecialActorIDOffset();
    const ptrdiff_t SpecialActorData = AFortGameStateAthena::SpecialActorDataOffset();
    if (!StaticClass() || SpawnerSpecialActorID <= 0 || CraftSpecialActorID <= 0 || SpecialActorData <= 0)
    {
        return false;
    }
    return Finder::ResolveSpecialActorReplicationLive(SpawnerSpecialActorID, CraftSpecialActorID, SpecialActorData);
}
