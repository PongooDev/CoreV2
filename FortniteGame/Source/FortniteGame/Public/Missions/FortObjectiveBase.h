#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"

class AFortMission;
class FText;
class UFortWorldItemDefinition;

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
    DefineUFunction(BlueprintOnStartPlaying)
    DefineUFunction(BlueprintOnObjectiveEnd)

public:
    DefineUProperty(TArray<UFortWorldItemDefinition*>, ItemsToGiveOnObjectiveStart)
    DefineBitfieldUProperty(bStartPlayingOnMissionStart)
    DefineUProperty(EFortObjectiveStatus, ObjectiveStatus)
    DefineUProperty(FGameplayTagContainer, ObjectiveHandle)

    AFortMission* GetMission() const;
    void SetObjectiveStatus(EFortObjectiveStatus NewStatus);
    bool ActivateObjective();
    bool StartPlayingObjective();
    void StopObjective(EFortObjectiveStatus Status, const FText& Description, UObject* ExtraBadgeToGrant);
    void ClearObjectiveTimer();
    void BlueprintOnStartPlaying();
    void BlueprintOnObjectiveEnd(EFortObjectiveStatus Status);
    void SendObjectiveEndedEvent(EFortObjectiveStatus Status);

    static void HookVTables();

private:
    static bool ActivateObjectiveHK(AFortObjectiveBase* This);
    static bool StartPlayingObjectiveHK(AFortObjectiveBase* This);
    static void StopObjectiveHK(AFortObjectiveBase* This, EFortObjectiveStatus Status, const FText& Description, UObject* ExtraBadgeToGrant);
};
