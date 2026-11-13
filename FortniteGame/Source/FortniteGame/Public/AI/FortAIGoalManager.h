#pragma once

#include "AI/FortAIEncounterInfo.h"
#include "Containers/Array.h"
#include "FortTeamInfo.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "ObjectLayout.h"
#include "StructArray.h"
#include "Templates/SharedPointer.h"
#include "UObject/Stack.h"
#include "UObject/WeakObjectPtr.h"

class AFortAIPawn;
class UObject;
struct FEnvQueryResult;

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

// A goal and the assignment it is of: UFortQueryItemType_Goal's value (21.40's name), FFortAIGoalInfo's 0x18 bytes
// (3.2 to 6.21) and then the assignment.
struct FFortAIAssignmentGoalInfo
{
    uint8_t GoalInfo[0x18];
    FWeakObjectPtr Assignment;
};
static_assert(sizeof(FFortAIAssignmentGoalInfo) == 0x20);

struct FAIAssignmentInfo
{
    DefineUnrealStruct(FAIAssignmentInfo, FortniteGame)
    DefineUStructProperty(FFortAIGoalInfo, CurrentGoal)
    DefineUProperty(bool, bSuppressGoalUpdates)
    // The goal query result that came while goal updates were suppressed (unreflected; Finder::ResolveGoalQueryResultLive).
    DefineNativeMember(TSharedPtr<FEnvQueryResult>, LastQueryResult, FAIAssignmentInfo_LastQueryResult)

    static ptrdiff_t SuppressGoalUpdatesOffset() { return Member_bSuppressGoalUpdates.IsAvailable() ? Member_bSuppressGoalUpdates.GetOffsetChecked() : -1; }

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
    DefineUProperty(AFortAIPawn*, MyFortPawn)

    static bool HasMyFortPawn() { return Member_MyFortPawn.IsAvailable(); }
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

    // The goals AI can take (FFortAIGoal::CanAIUse with the assignment's settings) as goal items of QueryInstance (an
    // FEnvQueryInstance), or added to OutGoals, each with this assignment (6.20 CL-4497486 0xA87D60 / 0xA87B90). Epic has
    // them in FortAIAssignment.cpp; Core defines them beside its EQS goal item helpers, in FortQueryGenerators.cpp and
    // FortQueryContexts.cpp.
    void AddValidGoalsForAIToEQI(AFortAIController* AI, AFortAIPawn* Pawn, void* QueryInstance);
    void AddValidGoalsForAI(AFortAIController* AI, AFortAIPawn* Pawn, TArray<FFortAIAssignmentGoalInfo>& OutGoals);
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

    // The goals Pawn's AI can take from the assignments Pawn can use: its encounter's, then the world's, as goal items of
    // QueryInstance (6.20 CL-4497486 0xAC6B20, UFortQueryGenerator_AssignmentGoal's) or added to OutGoals (0xAC6A20,
    // UFortQueryContext_AllGoals'). Core's names: the 21.40 PDB doesn't name these.
    void AddValidGoalsForAIToEQI(AFortAIPawn* Pawn, void* QueryInstance);
    void AddValidGoalsForAI(AFortAIPawn* Pawn, TArray<FFortAIAssignmentGoalInfo>& OutGoals);
    // UFortBTContext_SuppressGoalUpdate's (6.20 0xAD2850, Core's name): while suppressed, goal query results only wait in
    // the AI's LastQueryResult; lifting it handles the one that came, if the AI still has its pawn.
    void SuppressGoalUpdates(AFortAIController* AI, bool bSuppress);
    // The build's own (Finder::ResolveGoalDetectionLive): adds or removes the AI's FAIAssignmentInfo.
    void RegisterAIForGoalDetection(AFortAIController* AI);
    void UnregisterAIForGoalDetection(AFortAIController* AI);
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

private:
    // One list of assignments' part of AddValidGoalsForAIToEQI / AddValidGoalsForAI (6.20 0xABAEA0 / 0xABADD0).
    void AddValidGoalsForAIToEQI(AFortAIPawn* Pawn, const TArray<UObject*>& Assignments, void* QueryInstance);
    void AddValidGoalsForAI(AFortAIPawn* Pawn, const TArray<UObject*>& Assignments, TArray<FFortAIAssignmentGoalInfo>& OutGoals);
};
