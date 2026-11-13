#pragma once

#include "AI/FortBTTask_SetFrustrationDiscouragement.h"
#include "UObject/Object.h"

class UFortBTService_ContextOverride : public UObject
{
    DefineUnrealClass(UFortBTService_ContextOverride, FortniteGame)
};

// The behavior tree service a branch of the husks' trees runs under: while the branch is relevant the AI's goal manager
// holds back new goals for it (or forgets the AI, with bUnregisterFromGoalManager), and lets them through again after.
class UFortBTContext_SuppressGoalUpdate : public UFortBTService_ContextOverride
{
    DefineUnrealClass(UFortBTContext_SuppressGoalUpdate, FortniteGame)

public:
    DefineUProperty(bool, bUnregisterFromGoalManager)

    // OnBecomeRelevant's and OnCeaseRelevant's (6.20 CL-4497486 0xAE5C10 / 0xB046C0, slots 0x56 / 0x57). 6.21 CL-4526925
    // (both 0xADC3E0) and 3.2 (both 0x8987A0) keep only the cast before AFortAIGoalManager::GetCurrent; Core puts these
    // in their slots.
    void ApplyValueOverrides(UBrainComponent& OwnerComp, uint8_t* NodeMemory) const;
    void RevertValueOverrides(UBrainComponent& OwnerComp, uint8_t* NodeMemory) const;

    static void HookVTables();

private:
    static void ApplyValueOverridesHK(UFortBTContext_SuppressGoalUpdate* This, UBrainComponent& OwnerComp, uint8_t* NodeMemory);
    static void RevertValueOverridesHK(UFortBTContext_SuppressGoalUpdate* This, UBrainComponent& OwnerComp, uint8_t* NodeMemory);
};
