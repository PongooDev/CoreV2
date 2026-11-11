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
#include "Containers/ScriptArray.h"
#include "Math/Vector.h"
#include "UObject/WeakObjectPtr.h"

#include <cstring>
#include <format>
#include <string>
#include <unordered_map>

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

namespace
{
    // The encounter's assignments as its primary assignment lookup sees them: type, team and goals (the first goal's actor
    // or location). Its spawn queries (EncounterMinDistanceNavGraph and the rest) generate around and measure from the
    // primary assignment's goals (FortQueryContext_EncounterTargetObjective), so an encounter that spawns no husks shows
    // here whether it had any.
    std::wstring DescribeEncounterAssignments(const AFortAIGoalManager* GoalManager, UFortAIEncounterInfo* Encounter)
    {
        TArray<UObject*>& Assignments = Encounter->EncounterAssignments;
        std::wstring Description = std::format(L"goal manager {}, {} assignments", GoalManager ? GoalManager->GetName().ToStdString() : std::wstring(L"none"), Assignments.Num());
        const int32_t GoalSize = FFortAIGoal::StaticStruct() ? FFortAIGoal::StaticStruct()->GetStructureSize() : 0;
        for (UObject* Assignment : Assignments)
        {
            if (!Assignment)
            {
                Description += L"; null";
                continue;
            }
            Description += L"; " + Assignment->GetName().ToStdString();
            if (UFortAIAssignment::HasAssignmentIdentifier())
            {
                const FFortAIAssignmentIdentifier& Identifier = static_cast<UFortAIAssignment*>(Assignment)->AssignmentIdentifier;
                Description += std::format(L" ({}, team {})", Identifier.AssignmentType.ToString(), Identifier.AssignmentTeam.ToString());
            }
            if (Natives::UFortAIAssignment_Goals < 0 || GoalSize <= 0)
            {
                continue;
            }
            const FScriptArray& Goals = *reinterpret_cast<const FScriptArray*>(reinterpret_cast<uint8_t*>(Assignment) + Natives::UFortAIAssignment_Goals);
            Description += std::format(L" {} goals", Goals.Num());
            if (Goals.Num() > 0)
            {
                const uint8_t* Goal = static_cast<const uint8_t*>(Goals.GetData());
                const UObject* Actor = reinterpret_cast<const FWeakObjectPtr*>(Goal)->Get();
                const FVector& Location = *reinterpret_cast<const FVector*>(Goal + 8);
                Description += Actor ? std::format(L", first {}", Actor->GetName().ToStdString()) : std::format(L", first at ({:.0f}, {:.0f}, {:.0f})", Location.X, Location.Y, Location.Z);
            }
        }
        return Description;
    }
}

AFortAIGoalManager* AFortAIGoalManager::GetCurrentForEncounterAssignments(const UObject* EncounterInfo)
{
    AFortAIGoalManager* GoalManager = GetCurrent(EncounterInfo);
#if WITH_SERVER_CODE
    static std::unordered_map<const UObject*, std::wstring> LastDescriptions;
    if (UFortAIEncounterInfo* Encounter = Cast<UFortAIEncounterInfo>(const_cast<UObject*>(EncounterInfo)))
    {
        std::wstring Description = DescribeEncounterAssignments(GoalManager, Encounter);
        std::wstring& Last = LastDescriptions[EncounterInfo];
        if (Last != Description)
        {
            Log::Info(L"AFortAIGoalManager: encounter {} primary assignment lookup: {}", Encounter->GetName().ToStdString(), Description);
            Last = std::move(Description);
        }
    }
#endif
    return GoalManager;
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

    const EAssignmentType Type = Identifier.AssignmentType;
    const uint8_t TypeValue = static_cast<uint8_t>(Type);
    if (Type == EAssignmentType::GetEncounter())
    {
        UFortAIEncounterInfo* Encounter = Cast<UFortAIEncounterInfo>(EncounterInfo);
        if (!Encounter)
        {
            return nullptr;
        }
        TArray<UObject*>& Assignments = Encounter->EncounterAssignments;
        return Natives::AFortAIGoalManager_FindAssignmentInList(this, &Identifier, &Assignments, TypeValue - 1);
    }
    if (Type == EAssignmentType::GetWorld())
    {
        TArray<UObject*>& Assignments = WorldAssignments;
        return Natives::AFortAIGoalManager_FindAssignmentInList(this, &Identifier, &Assignments, TypeValue - 2);
    }
    if (Type == EAssignmentType::GetEnemy())
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
    std::vector<uint8_t*> EncounterCalls;
    for (uint8_t* Call : Finder::GoalManagerGetCurrentCalls(GoalManagerVTable, DirectorVTable, StaticClass()->GetPropertiesSize(), AFortAIDirector::StaticClass()->GetPropertiesSize(), AActor::StaticClass()->GetPropertiesSize(), Natives::UObject_GetWorldIndex, UFortAIEncounterInfo::EncounterAssignmentsOffset(), Stub, DirectorCalls, EncounterCalls))
    {
        Hooks::RedirectCall(L"AFortAIGoalManager::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrent));
    }
    // 5.0+: the encounter's primary assignment lookup (FindAssignment inlined), which its spawn queries' contexts use.
    for (uint8_t* Call : EncounterCalls)
    {
        Hooks::RedirectCall(L"AFortAIGoalManager::GetCurrent (encounter assignments)", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrentForEncounterAssignments));
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
