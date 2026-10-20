#pragma once

#include "GameFramework/Actor.h"

class AFortMission;
class FText;

enum class EFortObjectiveStatus : uint8_t
{
    Created,
    InProgress,
    Succeeded,
    Failed,
    NeutralCompletion,
    Max_None,
};

class AFortObjectiveBase : public AActor
{
    DefineUnrealClass(AFortObjectiveBase, FortniteGame)
    DefineUFunction(StartPlayingObjective)
    DefineUFunction(StopObjective)
    DefineUFunction(ClearObjectiveTimer)
    DefineUFunction(OnRep_ObjectiveStatus)

public:
    DefineUProperty(EFortObjectiveStatus, ObjectiveStatus)

    AFortMission* GetMission() const;
    void SetObjectiveStatus(EFortObjectiveStatus NewStatus);
    bool StartPlayingObjective();
    void StopObjective(EFortObjectiveStatus Status, const FText& Description, UObject* ExtraBadgeToGrant);
    void ClearObjectiveTimer();

    static void HookVTables();

private:
    static bool StartPlayingObjectiveHK(AFortObjectiveBase* This);
    static void StopObjectiveHK(AFortObjectiveBase* This, EFortObjectiveStatus Status, const FText& Description, UObject* ExtraBadgeToGrant);
};
