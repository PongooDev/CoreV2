#pragma once

#include "AI/FortAIEncounterInfo.h"
#include "Containers/Array.h"
#include "FortTeamInfo.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "StructArray.h"
#include "UObject/Stack.h"
#include "UObject/WeakObjectPtr.h"

class UObject;

class EAssignmentType
{
    DefineUnrealEnum(EAssignmentType, FortniteGame)
    DefineUEnumValue(Invalid)
    DefineUEnumValue(Encounter)
    DefineUEnumValue(World)
    DefineUEnumValue(Enemy)
    DefineUEnumValue(NumAssignmentTypes)
};

struct FFortAIAssignmentIdentifier
{
    DefineUnrealStruct(FFortAIAssignmentIdentifier, FortniteGame)
    DefineUProperty(EAssignmentType, AssignmentType)
    DefineUProperty(EFortTeam, AssignmentTeam)
};

struct FFortAIGoal
{
    DefineUnrealStruct(FFortAIGoal, FortniteGame)
};

struct FFortAIGoalInfo
{
    DefineUnrealStruct(FFortAIGoalInfo, FortniteGame)
    DefineUProperty(FWeakObjectPtr, Actor)
};

struct FAIDiscouragedGoalTimer
{
    DefineUnrealStruct(FAIDiscouragedGoalTimer, FortniteGame)
    DefineUStructProperty(FFortAIGoalInfo, DiscouragedGoalInfo)
    DefineUProperty(double, ExpirationTime)
    DefineUProperty(uint32_t, NumberOfTimesMarkedForDiscouragement)
};

struct FAIAssignmentInfo
{
    DefineUnrealStruct(FAIAssignmentInfo, FortniteGame)
    DefineUStructProperty(FFortAIGoalInfo, CurrentGoal)

    TStructArray<FAIDiscouragedGoalTimer>& GetDiscouragedGoalTimers() const;
    void ClearExpiredFrustrationDiscouragements(double CurrentTime);
};

class AAIController : public AController
{
    DefineUnrealClass(AAIController, AIModule)

public:
    DefineUProperty(UObject*, PerceptionComponent)
};

class AFortAIController : public AAIController
{
    DefineUnrealClass(AFortAIController, FortniteGame)
    DefineUFunction(GetEncounterInfo)

public:
    UObject* GetEncounterInfo();
};

class UFortAIAssignmentSettings : public UObject
{
    DefineUnrealClass(UFortAIAssignmentSettings, FortniteGame)

public:
    DefineUProperty(float, MaxAIAllowedForAssignment)
    DefineUProperty(float, MaxAIAllowedPerGoal)

    static ptrdiff_t MaxAIAllowedForAssignmentOffset() { return Member_MaxAIAllowedForAssignment.IsAvailable() ? Member_MaxAIAllowedForAssignment.GetOffsetChecked() : -1; }
    static ptrdiff_t MaxAIAllowedPerGoalOffset() { return Member_MaxAIAllowedPerGoal.IsAvailable() ? Member_MaxAIAllowedPerGoal.GetOffsetChecked() : -1; }
};

class UFortAIAssignment : public UObject
{
    DefineUnrealClass(UFortAIAssignment, FortniteGame)

public:
    DefineUProperty(UObject*, AssignmentSettings)
    DefineUStructProperty(FFortAIAssignmentIdentifier, AssignmentIdentifier)

    static bool HasAssignmentIdentifier() { return Member_AssignmentIdentifier.IsAvailable(); }
    UObject* GetAssignmentSettings() const { return Member_AssignmentSettings.IsAvailable() ? AssignmentSettings : nullptr; }
};

class UFortAIGoalProvider : public UObject
{
    DefineUnrealClass(UFortAIGoalProvider, FortniteGame)

public:
    DefineUProperty(UObject*, EncounterInfo)

    UObject* GetEncounterInfo() const { return EncounterInfo; }
};

class UFortAIGoalProvider_EnvQuery : public UFortAIGoalProvider
{
    DefineUnrealClass(UFortAIGoalProvider_EnvQuery, FortniteGame)

public:
    DefineUStructProperty(FFortAIAssignmentIdentifier, SpecificAssignmentContext)

    const FFortAIAssignmentIdentifier& GetRootAssignmentIdentifier() const { return SpecificAssignmentContext; }
};

class AFortAIGoalManager : public AActor
{
    DefineUnrealClass(AFortAIGoalManager, FortniteGame)
    DefineUFunction(SetCurrentGoalDiscouragement)

public:
    DefineUProperty(TArray<UObject*>, WorldAssignments)
    DefineUProperty(TArray<UObject*>, WorldEnemyAssignments)

    UObject* FindAssignment(const FFortAIAssignmentIdentifier& Identifier, UObject* EncounterInfo);
    FAIAssignmentInfo* GetAssignmentInfo(AFortAIController* AI);
    // Marks the AI's current goal discouraged for DiscouragementDuration; false without its assignment info.
    bool SetFrustrationDiscouragement(AFortAIController* AI, float DiscouragementDuration);

    static AFortAIGoalManager* GetCurrent(const UObject* WorldContextObject);
    // GetCurrent for 5.0+'s encounter primary assignment lookup, which inlines FindAssignment (Finder::GoalManagerGetCurrentCalls).
    // Logs the encounter's assignments whenever they change, so a mission's spawn queries show what they measure from.
    static AFortAIGoalManager* GetCurrentForEncounterAssignments(const UObject* EncounterInfo);
    static void SetCurrentGoalDiscouragement(UObject* WorldContext, AFortAIController* AI);
    // Core's diagnostics, from the frustration triggers (GAB_Hunting's SetCurrentGoalDiscouragement, BT_HuskBase's
    // FortBTTask_SetFrustrationDiscouragement): the goal the AI gave up on, its perception and movement state and the
    // navigation path to the goal, rate-limited.
    static void LogFrustratedAI(AFortAIController* AI, const wchar_t* Reason);
    static void HookCallSites();
    static void HookNatives();

    DECLARE_FUNCTION(execSetCurrentGoalDiscouragement);
};
