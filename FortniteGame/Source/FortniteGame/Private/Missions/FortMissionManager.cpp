#include "pch.h"
#include "Missions/FortMissionManager.h"
#include "FortGameState.h"
#include "Finder.h"
#include "Natives.h"

void FFortMissionManagerRecord::Construct(FFortMissionManagerRecord* This, UClass* InMissionManagerClass)
{
    if (Natives::FFortMissionManagerRecord_FFortMissionManagerRecord || Finder::ResolveMissionManagerLive(AFortGameState::MissionManagerOffset()))
    {
        Natives::FFortMissionManagerRecord_FFortMissionManagerRecord(This, InMissionManagerClass);
    }
}
