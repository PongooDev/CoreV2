#pragma once

#include "AI/FortAIGoalManager.h"
#include "UObject/Object.h"

class AActor;

class UFortQueryGenerator_Enemies : public UObject
{
    DefineUnrealClass(UFortQueryGenerator_Enemies, FortniteGame)

public:
    DefineUProperty(bool, bPerceivedEnemiesOnly)

    void GenerateItems(void* QueryInstance) const;

    static void HookVTables();

private:
    static void GenerateItemsHK(UFortQueryGenerator_Enemies* This, void* QueryInstance);
};

class UFortQueryGenerator_SpecificAssignmentGoals : public UObject
{
    DefineUnrealClass(UFortQueryGenerator_SpecificAssignmentGoals, FortniteGame)

public:
    DefineUStructProperty(FFortAIAssignmentIdentifier, AssignmentIdentifier)

    void GenerateItems(void* QueryInstance) const;

private:
    friend class UFortQueryGenerator_Enemies;
    static void GenerateItemsHK(UFortQueryGenerator_SpecificAssignmentGoals* This, void* QueryInstance);
};
