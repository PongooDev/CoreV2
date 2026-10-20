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
