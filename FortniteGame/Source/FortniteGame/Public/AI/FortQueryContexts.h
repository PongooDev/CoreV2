#pragma once

#include "UObject/Object.h"

class UFortQueryItemType_Goal : public UObject
{
    DefineUnrealClass(UFortQueryItemType_Goal, FortniteGame)
};

class UFortAIEncounterInfoOwnerInterface : public UObject
{
    DefineUnrealClass(UFortAIEncounterInfoOwnerInterface, FortniteGame)
};

class UFortQueryContext_EncounterPrimaryAssignmentGoals : public UObject
{
    DefineUnrealClass(UFortQueryContext_EncounterPrimaryAssignmentGoals, FortniteGame)

public:
    void ProvideContext(void* QueryInstance, void* ContextData) const;

    static void HookVTables();

private:
    static void ProvideContextHK(UFortQueryContext_EncounterPrimaryAssignmentGoals* This, void* QueryInstance, void* ContextData);
};

class UFortQueryContext_GoalProviderRootAssignmentGoals : public UObject
{
    DefineUnrealClass(UFortQueryContext_GoalProviderRootAssignmentGoals, FortniteGame)

public:
    void ProvideContext(void* QueryInstance, void* ContextData) const;

private:
    friend class UFortQueryContext_EncounterPrimaryAssignmentGoals;
    static void ProvideContextHK(UFortQueryContext_GoalProviderRootAssignmentGoals* This, void* QueryInstance, void* ContextData);
};

class UFortQueryContext_AllGoals : public UObject
{
    DefineUnrealClass(UFortQueryContext_AllGoals, FortniteGame)

public:
    // Every goal the querying AI can take from the assignments its pawn can use (6.20 CL-4497486 0xB02740). 6.21
    // CL-4526925 (0xAF5820) and 3.2 (0x8B23C0) only hand an empty list to the context; Core puts this in their slot.
    void ProvideContext(void* QueryInstance, void* ContextData) const;

    static void HookVTables();

private:
    static void ProvideContextHK(UFortQueryContext_AllGoals* This, void* QueryInstance, void* ContextData);
};
