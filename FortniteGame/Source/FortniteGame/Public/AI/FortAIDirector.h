#pragma once

#include "GameFramework/Actor.h"
#include "Missions/FortObjectiveBase.h"

class UFortAIEncounterInfo;

class AFortAIDirector : public AActor
{
    DefineUnrealClass(AFortAIDirector, FortniteGame)

public:
    static AFortAIDirector* GetCurrent(const UObject* WorldContextObject);

    void StopEncounter(UFortAIEncounterInfo* EncounterToStop, EFortObjectiveStatus EncounterStopReason, bool bForceKillAI = false, bool bBroadcastStop = true);
};
