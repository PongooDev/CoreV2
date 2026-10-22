#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"
#include "StructArray.h"
#include "UObject/Stack.h"
#include "UObject/WeakObjectPtr.h"

class UObject;

enum class EFortAIAssignmentType : uint8_t
{
    None,
    Encounter,
    World,
    WorldEnemy,
};

struct FFortAIAssignmentIdentifier
{
    DefineUnrealStruct(FFortAIAssignmentIdentifier, FortniteGame)
    DefineUProperty(EFortAIAssignmentType, AssignmentType)
    DefineUProperty(uint8_t, AssignmentTeam)
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

class UFortAIEncounterInfo : public UObject
{
    DefineUnrealClass(UFortAIEncounterInfo, FortniteGame)

public:
    DefineUProperty(TArray<UObject*>, EncounterAssignments)
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
    void SetFrustrationDiscouragement(AFortAIController* AI, float DiscouragementDuration);

    static AFortAIGoalManager* GetCurrent(const UObject* WorldContextObject);
    static void SetCurrentGoalDiscouragement(UObject* WorldContext, AFortAIController* AI);
    static void HookCallSites();
    static void HookNatives();

    DECLARE_FUNCTION(execSetCurrentGoalDiscouragement);
};
