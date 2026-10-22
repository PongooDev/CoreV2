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

#include <cstring>

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

TStructArray<FAIDiscouragedGoalTimer>& FAIAssignmentInfo::GetDiscouragedGoalTimers() const
{
    return *reinterpret_cast<TStructArray<FAIDiscouragedGoalTimer>*>(reinterpret_cast<uintptr_t>(this) + Natives::FAIAssignmentInfo_DiscouragedGoalTimers);
}

void FAIAssignmentInfo::ClearExpiredFrustrationDiscouragements(double CurrentTime)
{
    TStructArray<FAIDiscouragedGoalTimer>& Timers = GetDiscouragedGoalTimers();
    for (int32_t Index = Timers.Num() - 1; Index >= 0; --Index)
    {
        if (Timers[Index].ExpirationTime <= CurrentTime)
        {
            Timers.RemoveAt(Index);
        }
    }
}

FAIAssignmentInfo* AFortAIGoalManager::GetAssignmentInfo(AFortAIController* AI)
{
    if (!AI || !Finder::ResolveGoalDiscouragementLive())
    {
        return nullptr;
    }
    return static_cast<FAIAssignmentInfo*>(Natives::AFortAIGoalManager_GetAssignmentInfo(this, AI));
}

void AFortAIGoalManager::SetFrustrationDiscouragement(AFortAIController* AI, float DiscouragementDuration)
{
#if WITH_SERVER_CODE
    FAIAssignmentInfo* AssignmentInfo = GetAssignmentInfo(AI);
    UWorld* World = AssignmentInfo ? GetWorld() : nullptr;
    if (!World)
    {
        return;
    }

    const double CurrentTime = World->GetTimeSeconds();
    AssignmentInfo->ClearExpiredFrustrationDiscouragements(CurrentTime);

    FFortAIGoalInfo& CurrentGoal = AssignmentInfo->CurrentGoal;
    TStructArray<FAIDiscouragedGoalTimer>& Timers = AssignmentInfo->GetDiscouragedGoalTimers();
    for (int32_t Index = 0; Index < Timers.Num(); ++Index)
    {
        FAIDiscouragedGoalTimer& Timer = Timers[Index];
        FFortAIGoalInfo& DiscouragedGoalInfo = Timer.DiscouragedGoalInfo;
        if (DiscouragedGoalInfo.Actor == CurrentGoal.Actor)
        {
            Timer.ExpirationTime = CurrentTime + DiscouragementDuration;
            Timer.NumberOfTimesMarkedForDiscouragement = Timer.NumberOfTimesMarkedForDiscouragement + 1;
            return;
        }
    }

    FAIDiscouragedGoalTimer& Timer = Timers.AddDefaulted();
    FFortAIGoalInfo& DiscouragedGoalInfo = Timer.DiscouragedGoalInfo;
    std::memcpy(&DiscouragedGoalInfo, &CurrentGoal, FFortAIGoalInfo::StaticStruct()->GetStructureSize());
    Timer.ExpirationTime = CurrentTime + DiscouragementDuration;
    Timer.NumberOfTimesMarkedForDiscouragement = 1;
#endif
}

void AFortAIGoalManager::SetCurrentGoalDiscouragement(UObject* WorldContext, AFortAIController* AI)
{
#if WITH_SERVER_CODE
    constexpr float DefaultDiscouragementDuration = 60.f;
    if (AFortAIGoalManager* GoalManager = GetCurrent(WorldContext))
    {
        GoalManager->SetFrustrationDiscouragement(AI, DefaultDiscouragementDuration);
    }
#endif
}

DEFINE_FUNCTION(AFortAIGoalManager::execSetCurrentGoalDiscouragement)
{
    P_GET_OBJECT(UObject, Z_Param_WorldContext);
    P_GET_OBJECT(AFortAIController, Z_Param_AI);
    P_FINISH;
    P_NATIVE_BEGIN;
    AFortAIGoalManager::SetCurrentGoalDiscouragement(Z_Param_WorldContext, Z_Param_AI);
    P_NATIVE_END;
}

void AFortAIGoalManager::HookNatives()
{
    if (UFunction* Function = StaticFunction_SetCurrentGoalDiscouragement(); Function && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        Function->SetNativeFunc(&execSetCurrentGoalDiscouragement);
        Log::Info(L"Hook AFortAIGoalManager::SetCurrentGoalDiscouragement: native");
    }
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
    std::vector<uint8_t*> DirectorCalls;
    for (uint8_t* Call : Finder::GoalManagerGetCurrentCalls(GoalManagerVTable, DirectorVTable, StaticClass()->GetPropertiesSize(), AFortAIDirector::StaticClass()->GetPropertiesSize(), AActor::StaticClass()->GetPropertiesSize(), Natives::UObject_GetWorldIndex, Stub, DirectorCalls))
    {
        Hooks::RedirectCall(L"AFortAIGoalManager::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrent));
    }
    for (uint8_t* Call : DirectorCalls)
    {
        Hooks::RedirectCall(L"AFortAIDirector::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIDirector::GetCurrent));
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
