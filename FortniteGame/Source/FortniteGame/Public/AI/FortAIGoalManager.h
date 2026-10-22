#pragma once

#include "Containers/Array.h"
#include "GameFramework/Actor.h"
#include "GameFramework/Controller.h"

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

public:
    DefineUProperty(TArray<UObject*>, WorldAssignments)
    DefineUProperty(TArray<UObject*>, WorldEnemyAssignments)

    UObject* FindAssignment(const FFortAIAssignmentIdentifier& Identifier, UObject* EncounterInfo);

    static AFortAIGoalManager* GetCurrent(const UObject* WorldContextObject);
    static void HookCallSites();
};
