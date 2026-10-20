#include "pch.h"
#include "AI/FortAIGoalManager.h"
#include "AI/FortAIDirector.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

AFortAIGoalManager* AFortAIGoalManager::GetCurrent(const UObject* WorldContextObject)
{
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    AFortGameModeZone* GameMode = World ? Cast<AFortGameModeZone>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    return GameMode ? GameMode->AIGoalManager : nullptr;
#else
    return nullptr;
#endif
}

UObject* AFortAIController::GetEncounterInfo()
{
    Reflection::FParams Params(StaticFunction_GetEncounterInfo());
    return Params.Call(this) ? Params.Take<UObject*>(L"ReturnValue") : nullptr;
}

UObject* AFortAIGoalManager::FindAssignment(const FFortAIAssignmentIdentifier& Identifier, UObject* EncounterInfo)
{
    if (!Natives::AFortAIGoalManager_FindAssignmentInList)
    {
        return nullptr;
    }

    const EFortAIAssignmentType Type = Identifier.AssignmentType;
    const uint8_t TypeValue = static_cast<uint8_t>(Type);
    if (Type == EFortAIAssignmentType::Encounter)
    {
        UFortAIEncounterInfo* Encounter = Cast<UFortAIEncounterInfo>(EncounterInfo);
        if (!Encounter)
        {
            return nullptr;
        }
        TArray<UObject*>& Assignments = Encounter->EncounterAssignments;
        return Natives::AFortAIGoalManager_FindAssignmentInList(this, &Identifier, &Assignments, TypeValue - 1);
    }
    if (Type == EFortAIAssignmentType::World)
    {
        TArray<UObject*>& Assignments = WorldAssignments;
        return Natives::AFortAIGoalManager_FindAssignmentInList(this, &Identifier, &Assignments, TypeValue - 2);
    }
    if (Type == EFortAIAssignmentType::WorldEnemy)
    {
        TArray<UObject*>& Assignments = WorldEnemyAssignments;
        return Natives::AFortAIGoalManager_FindAssignmentInList(this, &Identifier, &Assignments, TypeValue - 2);
    }
    return nullptr;
}

void AFortAIGoalManager::HookCallSites()
{
    UObject* GoalManagerDefault = StaticClass()->GetDefaultObject(false);
    UObject* DirectorDefault = AFortAIDirector::StaticClass()->GetDefaultObject(false);
    void** GoalManagerVTable = GoalManagerDefault ? *reinterpret_cast<void***>(GoalManagerDefault) : nullptr;
    void** DirectorVTable = DirectorDefault ? *reinterpret_cast<void***>(DirectorDefault) : nullptr;

    uintptr_t Stub = 0;
    for (uint8_t* Call : Finder::GoalManagerGetCurrentCalls(GoalManagerVTable, DirectorVTable, StaticClass()->GetPropertiesSize(), AActor::StaticClass()->GetPropertiesSize(), Stub))
    {
        Hooks::RedirectCall(L"AFortAIGoalManager::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrent));
    }

    for (const wchar_t* FunctionPath : {
             L"/Script/FortniteGame.FortMission:AddGoalToEncounterAssignment",
             L"/Script/FortniteGame.FortMission:AddGoalsToEncounterAssignment",
             L"/Script/FortniteGame.FortMission:CreateEncounterAssignment",
             L"/Script/FortniteGame.FortMission:FindGoalLocationsForEncounterAssignment",
             L"/Script/FortniteGame.FortMission:RemoveGoalFromEncounterAssignment",
             L"/Script/FortniteGame.FortMission:RemoveGoalsFromEncounterAssignment" })
    {
        UFunction* Function = FindObject<UFunction>(nullptr, FunctionPath);
        if (uint8_t* Call = Stub && Function ? Finder::FirstCallInImplementation(Function, Stub) : nullptr)
        {
            Hooks::RedirectCall(L"AFortAIGoalManager::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrent));
        }
    }

    Finder::ResolveGoalManagerLive();
}
