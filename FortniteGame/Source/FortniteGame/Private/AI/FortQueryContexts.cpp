#include "pch.h"
#include "AI/FortQueryContexts.h"
#include "AI/FortAIGoalManager.h"
#include "Containers/ScriptArray.h"
#include "Engine/World.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

#include <cstring>
#include <vector>

namespace
{
    constexpr size_t GoalInfoSize = 0x18;

    struct FFortGoalItem
    {
        uint8_t GoalInfo[GoalInfoSize];
        FWeakObjectPtr Assignment;
    };
    static_assert(sizeof(FFortGoalItem) == 0x20);

    UObject* QueryOwnerOf(void* QueryInstance)
    {
        if (Natives::FEnvQueryInstance_Owner < 0)
        {
            return nullptr;
        }
        return reinterpret_cast<const FWeakObjectPtr*>(static_cast<uint8_t*>(QueryInstance) + Natives::FEnvQueryInstance_Owner)->Get();
    }

    void AddAssignmentGoals(TArray<FFortGoalItem>& Goals, UObject* Assignment)
    {
        if (!Assignment || Natives::UFortAIAssignment_Goals < 0)
        {
            return;
        }

        const FScriptArray& AssignmentGoals = *reinterpret_cast<const FScriptArray*>(reinterpret_cast<uint8_t*>(Assignment) + Natives::UFortAIAssignment_Goals);
        const int32_t GoalSize = FFortAIGoal::StaticStruct()->GetStructureSize();
        for (int32_t Index = 0; Index < AssignmentGoals.Num(); ++Index)
        {
            FFortGoalItem Item;
            std::memcpy(Item.GoalInfo, static_cast<const uint8_t*>(AssignmentGoals.GetData()) + static_cast<size_t>(Index) * GoalSize, GoalInfoSize);
            Item.Assignment = Assignment;
            Goals.Add(Item);
        }
    }

    void SetGoalContext(void* ContextData, const TArray<FFortGoalItem>& Goals)
    {
        Natives::UFortQueryItemType_Goal_SetContextHelper(ContextData, reinterpret_cast<const FScriptArray&>(Goals));
    }
}

void UFortQueryContext_EncounterPrimaryAssignmentGoals::ProvideContext(void* QueryInstance, void* ContextData) const
{
#if WITH_SERVER_CODE
    UObject* QueryOwner = QueryOwnerOf(QueryInstance);
    if (!QueryOwner)
    {
        return;
    }

    TArray<FFortGoalItem> Goals;
    void* EncounterInfoOwner = Natives::UObject_GetInterfaceAddress(QueryOwner, UFortAIEncounterInfoOwnerInterface::StaticClass());
    if (EncounterInfoOwner)
    {
        if (UObject* EncounterInfo = CallVirtual<UObject*>(EncounterInfoOwner, Natives::IFortAIEncounterInfoOwnerInterface_GetEncounterInfoIndex))
        {
            if (AFortAIGoalManager* GoalManager = AFortAIGoalManager::GetCurrent(QueryOwner))
            {
                UScriptStruct* IdentifierStruct = FFortAIAssignmentIdentifier::StaticStruct();
                std::vector<uint8_t> IdentifierStorage(IdentifierStruct->GetStructureSize(), 0);
                FFortAIAssignmentIdentifier* PrimaryIdentifier = reinterpret_cast<FFortAIAssignmentIdentifier*>(IdentifierStorage.data());
                IdentifierStruct->InitializeStruct(PrimaryIdentifier);
                PrimaryIdentifier->AssignmentType = EFortAIAssignmentType::Encounter;
                PrimaryIdentifier->AssignmentTeam = 1;
                AddAssignmentGoals(Goals, GoalManager->FindAssignment(*PrimaryIdentifier, EncounterInfo));
                IdentifierStruct->DestroyStruct(PrimaryIdentifier);
            }
            else
            {
                Log::Warning(L"Unable to get specific assignment goals for UFortQueryContext_EncounterPrimaryAssignmentGoals.  No Goal Manager found!  Owner ({}) is an Encounter Info Owner, and the Encounter exists.", QueryOwner->GetName().ToStdString());
            }
        }
        else
        {
            Log::Warning(L"Unable to find encounter info for UFortQueryContext_EncounterPrimaryAssignmentGoals.  Owner ({}) is an Encounter Info Owner, but its EncounterInfo is nullptr.", QueryOwner->GetName().ToStdString());
        }
    }
    else
    {
        Log::Warning(L"Unable to find encounter info for UFortQueryContext_EncounterPrimaryAssignmentGoals.  Owner is {}", QueryOwner->GetName().ToStdString());
    }
    SetGoalContext(ContextData, Goals);
#endif
}

void UFortQueryContext_GoalProviderRootAssignmentGoals::ProvideContext(void* QueryInstance, void* ContextData) const
{
#if WITH_SERVER_CODE
    UFortAIGoalProvider_EnvQuery* GoalProvider = Cast<UFortAIGoalProvider_EnvQuery>(QueryOwnerOf(QueryInstance));
    if (!GoalProvider)
    {
        return;
    }

    AFortAIGoalManager* GoalManager = AFortAIGoalManager::GetCurrent(GoalProvider->GetWorld());
    TArray<FFortGoalItem> Goals;
    if (GoalManager)
    {
        AddAssignmentGoals(Goals, GoalManager->FindAssignment(GoalProvider->GetRootAssignmentIdentifier(), GoalProvider->GetEncounterInfo()));
    }
    SetGoalContext(ContextData, Goals);
#endif
}

void UFortQueryContext_EncounterPrimaryAssignmentGoals::HookVTables()
{
    UObject* EncounterPrimaryDefault = StaticClass()->GetDefaultObject(false);
    UObject* GoalProviderRootDefault = UFortQueryContext_GoalProviderRootAssignmentGoals::StaticClass()->GetDefaultObject(false);
    void** EncounterPrimaryVTable = EncounterPrimaryDefault ? *reinterpret_cast<void***>(EncounterPrimaryDefault) : nullptr;
    void** GoalProviderRootVTable = GoalProviderRootDefault ? *reinterpret_cast<void***>(GoalProviderRootDefault) : nullptr;
    if (!EncounterPrimaryVTable || !GoalProviderRootVTable || !Finder::ResolveQueryContextsLive(EncounterPrimaryVTable, UFortQueryItemType_Goal::StaticClass()))
    {
        return;
    }

    const int32_t Index = Natives::UEnvQueryContext_ProvideContextIndex;
    Hooks::SwapVTable(L"UFortQueryContext_EncounterPrimaryAssignmentGoals::ProvideContext", EncounterPrimaryVTable, Index, reinterpret_cast<void*>(&ProvideContextHK));
    Hooks::SwapVTable(L"UFortQueryContext_GoalProviderRootAssignmentGoals::ProvideContext", GoalProviderRootVTable, Index, reinterpret_cast<void*>(&UFortQueryContext_GoalProviderRootAssignmentGoals::ProvideContextHK));
}

void UFortQueryContext_EncounterPrimaryAssignmentGoals::ProvideContextHK(UFortQueryContext_EncounterPrimaryAssignmentGoals* This, void* QueryInstance, void* ContextData)
{
    This->ProvideContext(QueryInstance, ContextData);
}

void UFortQueryContext_GoalProviderRootAssignmentGoals::ProvideContextHK(UFortQueryContext_GoalProviderRootAssignmentGoals* This, void* QueryInstance, void* ContextData)
{
    This->ProvideContext(QueryInstance, ContextData);
}
