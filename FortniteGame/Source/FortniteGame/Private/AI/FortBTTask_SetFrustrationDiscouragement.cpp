#include "pch.h"
#include "AI/FortBTTask_SetFrustrationDiscouragement.h"
#include "AI/FortAIGoalManager.h"
#include "Finder.h"
#include "Hooks.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

EBTNodeResult UFortBTTask_SetFrustrationDiscouragement::ExecuteTask(UBrainComponent& OwnerComp, uint8_t* NodeMemory)
{
#if WITH_SERVER_CODE
    AFortAIController* AI = Cast<AFortAIController>(reinterpret_cast<UObject*>(static_cast<AAIController*>(OwnerComp.AIOwner)));
    AFortAIGoalManager* GoalManager = AI ? AFortAIGoalManager::GetCurrent(AI) : nullptr;
    if (GoalManager)
    {
        GoalManager->SetFrustrationDiscouragement(AI, DiscouragementDuration);
        // BT_HuskBase's "Can't Reach Anything" sequence: the move to the goal failed.
        AFortAIGoalManager::LogFrustratedAI(AI, L"can't reach");
        return EBTNodeResult::Succeeded;
    }
#endif
    return EBTNodeResult::Failed;
}

void UFortBTTask_SetFrustrationDiscouragement::HookVTables()
{
    UObject* TaskDefault = StaticClass()->GetDefaultObject(false);
    void** TaskVTable = TaskDefault ? *reinterpret_cast<void***>(TaskDefault) : nullptr;
    const bool bDiscouragement = TaskVTable && Finder::ResolveGoalDiscouragementLive();
    // 5.0+ keeps Epic's ExecuteTask (only its AFortAIGoalManager::GetCurrent is stripped, which HookCallSites redirects).
    if (!TaskVTable || Finder::ResolveFrustrationDiscouragementLive(TaskVTable, DiscouragementDurationOffset()) || !bDiscouragement)
    {
        return;
    }
    Hooks::SwapVTable(L"UFortBTTask_SetFrustrationDiscouragement::ExecuteTask", TaskVTable, Natives::UBTTaskNode_ExecuteTaskIndex, reinterpret_cast<void*>(&ExecuteTaskHK));
}

EBTNodeResult UFortBTTask_SetFrustrationDiscouragement::ExecuteTaskHK(UFortBTTask_SetFrustrationDiscouragement* This, UBrainComponent& OwnerComp, uint8_t* NodeMemory)
{
    return This->ExecuteTask(OwnerComp, NodeMemory);
}
