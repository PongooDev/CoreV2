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
