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
#include "Pawns/FortAIPawn.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/WeakObjectPtr.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace
{
    constexpr size_t GoalInfoSize = sizeof(FFortAIAssignmentGoalInfo::GoalInfo);

    UObject* QueryOwnerOf(void* QueryInstance)
    {
        if (Natives::FEnvQueryInstance_Owner < 0)
        {
            return nullptr;
        }
        return reinterpret_cast<const FWeakObjectPtr*>(static_cast<uint8_t*>(QueryInstance) + Natives::FEnvQueryInstance_Owner)->Get();
    }

    void AddAssignmentGoals(TArray<FFortAIAssignmentGoalInfo>& Goals, UObject* Assignment)
    {
        if (!Assignment || Natives::UFortAIAssignment_Goals < 0)
        {
            return;
        }

        const FScriptArray& AssignmentGoals = *reinterpret_cast<const FScriptArray*>(reinterpret_cast<uint8_t*>(Assignment) + Natives::UFortAIAssignment_Goals);
        const int32_t GoalSize = FFortAIGoal::StaticStruct()->GetStructureSize();
        for (int32_t Index = 0; Index < AssignmentGoals.Num(); ++Index)
        {
            FFortAIAssignmentGoalInfo Item;
            std::memcpy(Item.GoalInfo, static_cast<const uint8_t*>(AssignmentGoals.GetData()) + static_cast<size_t>(Index) * GoalSize, GoalInfoSize);
            Item.Assignment = Assignment;
            Goals.Add(Item);
        }
    }

    void SetGoalContext(void* ContextData, const TArray<FFortAIAssignmentGoalInfo>& Goals)
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

    TArray<FFortAIAssignmentGoalInfo> Goals;
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
                PrimaryIdentifier->AssignmentType = EAssignmentType::GetEncounter();
                PrimaryIdentifier->AssignmentTeam = EFortTeam::GetMonster();
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
    TArray<FFortAIAssignmentGoalInfo> Goals;
    if (GoalManager)
    {
        AddAssignmentGoals(Goals, GoalManager->FindAssignment(GoalProvider->GetRootAssignmentIdentifier(), GoalProvider->GetEncounterInfo()));
    }
    SetGoalContext(ContextData, Goals);
#endif
}

void UFortAIAssignment::AddValidGoalsForAI(AFortAIController* AI, AFortAIPawn* Pawn, TArray<FFortAIAssignmentGoalInfo>& OutGoals)
{
#if WITH_SERVER_CODE
    if (Natives::UFortAIAssignment_Goals < 0 || !Natives::FFortAIGoal_CanAIUse)
    {
        return;
    }
    const FScriptArray& Goals = *reinterpret_cast<const FScriptArray*>(reinterpret_cast<uint8_t*>(this) + Natives::UFortAIAssignment_Goals);
    UObject* Settings = GetAssignmentSettings();
    const int32_t GoalSize = FFortAIGoal::StaticStruct()->GetStructureSize();
    for (int32_t Index = 0; Index < Goals.Num(); ++Index)
    {
        const uint8_t* Goal = static_cast<const uint8_t*>(Goals.GetData()) + static_cast<size_t>(Index) * GoalSize;
        if (Natives::FFortAIGoal_CanAIUse(Goal, reinterpret_cast<UObject*>(static_cast<AController*>(AI)), Settings))
        {
            FFortAIAssignmentGoalInfo Item;
            std::memcpy(Item.GoalInfo, Goal, GoalInfoSize);
            Item.Assignment = this;
            OutGoals.Add(Item);
        }
    }
#endif
}

void UFortQueryContext_AllGoals::ProvideContext(void* QueryInstance, void* ContextData) const
{
#if WITH_SERVER_CODE
    UObject* QueryOwner = QueryOwnerOf(QueryInstance);
    AFortAIPawn* Pawn = Cast<AFortAIPawn>(QueryOwner);
    AFortAIController* AI = Cast<AFortAIController>(Pawn ? reinterpret_cast<UObject*>(static_cast<AController*>(Pawn->Controller)) : QueryOwner);
    if (!AI)
    {
        return;
    }

    TArray<FFortAIAssignmentGoalInfo> Goals;
    if (AFortAIGoalManager* GoalManager = AFortAIGoalManager::GetCurrent(reinterpret_cast<UObject*>(static_cast<AController*>(AI))))
    {
        GoalManager->AddValidGoalsForAI(Pawn, Goals);
    }
    SetGoalContext(ContextData, Goals);

    // Core's diagnostics: what a pawn class's all-goals context held, once per pawn class and outcome.
    static std::set<std::wstring> Logged;
    const std::wstring PawnClass = Pawn ? reinterpret_cast<UObject*>(Pawn)->GetClass()->GetName().ToStdString() : std::wstring(L"none");
    if (Logged.size() < 200 && Logged.insert(PawnClass + (Goals.Num() ? L"|some" : L"|none")).second)
    {
        Log::Info(L"UFortQueryContext_AllGoals: {}'s goal context: {} goals from its encounter's and the world's assignments", PawnClass, Goals.Num());
    }
#endif
}

void UFortQueryContext_AllGoals::HookVTables()
{
    UClass* Class = StaticClass();
    UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
    void** VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    const int32_t Index = Natives::UEnvQueryContext_ProvideContextIndex;
    if (!VTable || Index < 0)
    {
        return;
    }

    // 6.02 to 6.20 keep Epic's body, whose AFortAIGoalManager::GetCurrent call HookCallSites redirects.
    const uintptr_t ProvideContext = reinterpret_cast<uintptr_t>(VTable[Index]);
    if (!Finder::IsFoldedGoalManagerUser(ProvideContext))
    {
        Log::Info(L"UFortQueryContext_AllGoals::ProvideContext (0x{:X}) is the build's own: it calls AFortAIGoalManager::GetCurrent", ProvideContext - Finder::ImageBase());
        return;
    }
    if (!Natives::UFortAIAssignment_CanPawnUse || !Natives::FFortAIGoal_CanAIUse || Natives::UFortAIAssignment_Goals < 0 || !Natives::UFortQueryItemType_Goal_SetContextHelper
        || !Natives::UObject_GetInterfaceAddress || Natives::IFortAIEncounterInfoOwnerInterface_GetEncounterInfoIndex < 0)
    {
        Log::Error(L"UFortQueryContext_AllGoals::ProvideContext (0x{:X}) lost its goal manager calls, but the assignment and goal checks, the goal context helper or the encounter lookup weren't found; not restored", ProvideContext - Finder::ImageBase());
        return;
    }
    Log::Info(L"UFortQueryContext_AllGoals::ProvideContext (0x{:X}) lost everything after AFortAIGoalManager::GetCurrent; Core restores it", ProvideContext - Finder::ImageBase());
    Hooks::SwapVTable(L"UFortQueryContext_AllGoals::ProvideContext", VTable, Index, reinterpret_cast<void*>(&ProvideContextHK));
}

void UFortQueryContext_AllGoals::ProvideContextHK(UFortQueryContext_AllGoals* This, void* QueryInstance, void* ContextData)
{
    This->ProvideContext(QueryInstance, ContextData);
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
