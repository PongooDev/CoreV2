#include "pch.h"
#include "AI/FortBTContext_SuppressGoalUpdate.h"
#include "AI/FortAIGoalManager.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

#include <set>
#include <string>

namespace
{
    // Core's diagnostics: that a husk's tree ran the restored service, once per AI class and action.
    void LogGoalUpdateOverride(AFortAIController* AI, const wchar_t* Action)
    {
        static std::set<std::wstring> Logged;
        const std::wstring AIClass = reinterpret_cast<UObject*>(static_cast<AController*>(AI))->GetClass()->GetName().ToStdString();
        if (Logged.size() < 100 && Logged.insert(AIClass + L"|" + Action).second)
        {
            Log::Info(L"UFortBTContext_SuppressGoalUpdate: {} {}", AIClass, Action);
        }
    }
}

void UFortBTContext_SuppressGoalUpdate::ApplyValueOverrides(UBrainComponent& OwnerComp, uint8_t* NodeMemory) const
{
#if WITH_SERVER_CODE
    AFortAIController* AI = Cast<AFortAIController>(reinterpret_cast<UObject*>(static_cast<AAIController*>(OwnerComp.AIOwner)));
    AFortAIGoalManager* GoalManager = AI ? AFortAIGoalManager::GetCurrent(reinterpret_cast<UObject*>(static_cast<AController*>(AI))) : nullptr;
    if (!GoalManager)
    {
        return;
    }
    if (bUnregisterFromGoalManager)
    {
        GoalManager->UnregisterAIForGoalDetection(AI);
        LogGoalUpdateOverride(AI, L"left the goal manager");
    }
    else
    {
        GoalManager->SuppressGoalUpdates(AI, true);
        LogGoalUpdateOverride(AI, L"holds its goal");
    }
#endif
}

void UFortBTContext_SuppressGoalUpdate::RevertValueOverrides(UBrainComponent& OwnerComp, uint8_t* NodeMemory) const
{
#if WITH_SERVER_CODE
    AFortAIController* AI = Cast<AFortAIController>(reinterpret_cast<UObject*>(static_cast<AAIController*>(OwnerComp.AIOwner)));
    AFortAIGoalManager* GoalManager = AI ? AFortAIGoalManager::GetCurrent(reinterpret_cast<UObject*>(static_cast<AController*>(AI))) : nullptr;
    if (!GoalManager)
    {
        return;
    }
    if (bUnregisterFromGoalManager)
    {
        GoalManager->RegisterAIForGoalDetection(AI);
        LogGoalUpdateOverride(AI, L"rejoined the goal manager");
    }
    else
    {
        GoalManager->SuppressGoalUpdates(AI, false);
        LogGoalUpdateOverride(AI, L"takes goals again");
    }
#endif
}

void UFortBTContext_SuppressGoalUpdate::HookVTables()
{
    UClass* Class = StaticClass();
    UClass* BaseClass = UFortBTService_ContextOverride::StaticClass();
    UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
    UObject* BaseDefault = BaseClass ? BaseClass->GetDefaultObject(false) : nullptr;
    void** VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    void** BaseVTable = BaseDefault ? *reinterpret_cast<void***>(BaseDefault) : nullptr;
    if (!VTable || !BaseVTable || !Finder::ResolveContextOverrideLive(BaseVTable))
    {
        return;
    }

    // 6.02 to 6.20 keep Epic's bodies, whose AFortAIGoalManager::GetCurrent calls HookCallSites redirects.
    const int32_t ApplyIndex = Natives::UFortBTService_ContextOverride_ApplyValueOverridesIndex;
    const int32_t RevertIndex = Natives::UFortBTService_ContextOverride_RevertValueOverridesIndex;
    const uintptr_t Apply = reinterpret_cast<uintptr_t>(VTable[ApplyIndex]);
    const uintptr_t Revert = reinterpret_cast<uintptr_t>(VTable[RevertIndex]);
    const bool bApplyFolded = VTable[ApplyIndex] != BaseVTable[ApplyIndex] && Finder::IsFoldedGoalManagerUser(Apply);
    const bool bRevertFolded = VTable[RevertIndex] != BaseVTable[RevertIndex] && Finder::IsFoldedGoalManagerUser(Revert);
    if (!bApplyFolded && !bRevertFolded)
    {
        Log::Info(L"UFortBTContext_SuppressGoalUpdate::ApplyValueOverrides / RevertValueOverrides (0x{:X} / 0x{:X}) are the build's own: they call AFortAIGoalManager::GetCurrent",
            Apply - Finder::ImageBase(), Revert - Finder::ImageBase());
        return;
    }

    UObject* AIControllerDefault = AFortAIController::StaticClass() ? AFortAIController::StaticClass()->GetDefaultObject(false) : nullptr;
    void** AIControllerVTable = AIControllerDefault ? *reinterpret_cast<void***>(AIControllerDefault) : nullptr;
    const bool bDetection = Finder::ResolveGoalDetectionLive(AIControllerVTable);
    const bool bQueryResult = Finder::ResolveGoalQueryResultLive(FAIAssignmentInfo::SuppressGoalUpdatesOffset());
    if (!bDetection || !bQueryResult || !Member_bUnregisterFromGoalManager.IsAvailable())
    {
        Log::Error(L"UFortBTContext_SuppressGoalUpdate::ApplyValueOverrides / RevertValueOverrides (0x{:X} / 0x{:X}) lost their goal manager calls, but the goal manager's registration or query result handling wasn't found; not restored",
            Apply - Finder::ImageBase(), Revert - Finder::ImageBase());
        return;
    }
    Log::Info(L"UFortBTContext_SuppressGoalUpdate::ApplyValueOverrides / RevertValueOverrides (0x{:X} / 0x{:X}) lost everything after AFortAIGoalManager::GetCurrent; Core restores them",
        Apply - Finder::ImageBase(), Revert - Finder::ImageBase());
    if (bApplyFolded)
    {
        Hooks::SwapVTable(L"UFortBTContext_SuppressGoalUpdate::ApplyValueOverrides", VTable, ApplyIndex, reinterpret_cast<void*>(&ApplyValueOverridesHK));
    }
    if (bRevertFolded)
    {
        Hooks::SwapVTable(L"UFortBTContext_SuppressGoalUpdate::RevertValueOverrides", VTable, RevertIndex, reinterpret_cast<void*>(&RevertValueOverridesHK));
    }
}

void UFortBTContext_SuppressGoalUpdate::ApplyValueOverridesHK(UFortBTContext_SuppressGoalUpdate* This, UBrainComponent& OwnerComp, uint8_t* NodeMemory)
{
    This->ApplyValueOverrides(OwnerComp, NodeMemory);
}

void UFortBTContext_SuppressGoalUpdate::RevertValueOverridesHK(UFortBTContext_SuppressGoalUpdate* This, UBrainComponent& OwnerComp, uint8_t* NodeMemory)
{
    This->RevertValueOverrides(OwnerComp, NodeMemory);
}
