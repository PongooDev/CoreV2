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

class UFortQueryGenerator_AssignmentGoal : public UObject
{
    DefineUnrealClass(UFortQueryGenerator_AssignmentGoal, FortniteGame)

public:
    // The goals of every assignment the querying AI's pawn can use (6.20 CL-4497486 0xAECD20). 6.21 CL-4526925 (0xAE4040)
    // and 3.2 (0x8A04B0) keep only the casts before AFortAIGoalManager::GetCurrent; Core puts this in their slot.
    void GenerateItems(void* QueryInstance) const;

    static void HookVTables();

private:
    static void GenerateItemsHK(UFortQueryGenerator_AssignmentGoal* This, void* QueryInstance);
};
