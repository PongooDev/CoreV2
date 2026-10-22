#pragma once

#include "UObject/Object.h"

class AAIController;

enum class EBTNodeResult : int32_t
{
    Succeeded,
    Failed,
    Aborted,
    InProgress,
};

class UBrainComponent : public UObject
{
    DefineUnrealClass(UBrainComponent, AIModule)

public:
    DefineUProperty(AAIController*, AIOwner)
};

class UBTTaskNode : public UObject
{
    DefineUnrealClass(UBTTaskNode, AIModule)
};

class UFortBTTask_SetFrustrationDiscouragement : public UBTTaskNode
{
    DefineUnrealClass(UFortBTTask_SetFrustrationDiscouragement, FortniteGame)

public:
    DefineUProperty(float, DiscouragementDuration)

    EBTNodeResult ExecuteTask(UBrainComponent& OwnerComp, uint8_t* NodeMemory);

    static void HookVTables();

private:
    static EBTNodeResult ExecuteTaskHK(UFortBTTask_SetFrustrationDiscouragement* This, UBrainComponent& OwnerComp, uint8_t* NodeMemory);
};
