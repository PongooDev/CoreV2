#include "pch.h"
#include "AI/FortAIGoalManager.h"
#include "AI/FortAIDirector.h"
#include "AI/Navigation/NavigationSystem.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
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
#include <functional>
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

    // Calls Object's reflected function by name and takes its ReturnValue; false when the build has no such function.
    template <typename T>
    bool CallReflected(UObject* Object, const wchar_t* FunctionName, T& OutValue, const std::function<void(Reflection::FParams&)>& SetParameters = nullptr)
    {
        const FName Name(FunctionName, FNAME_Find);
        UFunction* Function = Object && !Name.IsNone() ? Object->FindFunction(Name) : nullptr;
        if (!Function)
        {
            return false;
        }
        Reflection::FParams Params(Function, true);
        if (SetParameters)
        {
            SetParameters(Params);
        }
        if (!Params.Call(Object))
        {
            return false;
        }
        OutValue = Params.Take<T>(L"ReturnValue");
        return true;
    }

    UObject* ClassDefault(const wchar_t* ClassPath)
    {
        UClass* Class = FindObject<UClass>(nullptr, ClassPath);
        return Class ? Class->GetDefaultObject() : nullptr;
    }

    std::wstring Named(const UObject* Object)
    {
        return Object ? Object->GetName().ToStdString() : std::wstring(L"none");
    }

    // The AI's blackboard value KeyName (GoalActorIsVisible, GoalActorAlwaysPerceived), as text.
    std::wstring BlackboardBool(UObject* Blackboard, const wchar_t* KeyName)
    {
        bool bValue = false;
        const FName Key(KeyName, FNAME_Find);
        if (!Blackboard || Key.IsNone() || !CallReflected(Blackboard, L"GetValueAsBool", bValue, [&Key](Reflection::FParams& Params) { Params.Set(L"KeyName", Key); }))
        {
            return L"?";
        }
        return bValue ? L"yes" : L"no";
    }
}

void AFortAIGoalManager::LogFrustratedAI(AFortAIController* AI, const wchar_t* Reason)
{
#if WITH_SERVER_CODE
    // Core's diagnostics (#406), at the game's own frustration triggers: what an AI saw when it gave up on its goal, so a
    // match log shows whether the goal was perceived, whether the pawn moved and whether the navigation data had a path.
    // The first time an AI gives up on a goal, then every 20th time; at most 300 lines a server.
    struct FFrustration
    {
        const UObject* Goal = nullptr;
        int32_t Count = 0;
    };
    static std::unordered_map<const UObject*, FFrustration> Frustrations;
    static int32_t Lines = 0;
    APawn* Pawn = AI ? AI->GetPawn() : nullptr;
    if (!Pawn || Lines >= 300)
    {
        return;
    }

    AActor* GoalActor = nullptr;
    CallReflected(AI, L"GetGoalActor", GoalActor);
    FFrustration& Frustration = Frustrations[AI];
    Frustration.Count = Frustration.Goal == GoalActor ? Frustration.Count + 1 : 1;
    Frustration.Goal = GoalActor;
    if (Frustration.Count != 1 && Frustration.Count % 20 != 0)
    {
        return;
    }
    ++Lines;

    UObject* Blackboard = nullptr;
    if (UObject* Helpers = ClassDefault(L"/Script/AIModule.AIBlueprintHelperLibrary"))
    {
        CallReflected(Helpers, L"GetBlackboard", Blackboard, [AI](Reflection::FParams& Params) { Params.Set(L"Target", reinterpret_cast<AActor*>(static_cast<AController*>(AI))); });
    }
    UObject* Movement = nullptr;
    float MaxSpeed = -1.0f;
    CallReflected(Pawn, L"GetMovementComponent", Movement);
    CallReflected(Movement, L"GetMaxSpeed", MaxSpeed);
    uint8_t MoveStatus = 0xFF;
    CallReflected(AI, L"GetMoveStatus", MoveStatus);
    FVector PawnLocation(0, 0, 0);
    CallReflected(Pawn, L"K2_GetActorLocation", PawnLocation);

    // UNavigationSystem::FindPathToActorSynchronously with the pawn as the pathfinding context (its agent's navmesh).
    std::wstring PathText = L"no goal actor";
    UObject* Path = nullptr;
    if (GoalActor && CallReflected(UNavigationSystem::StaticClass()->GetDefaultObject(), L"FindPathToActorSynchronously", Path, [&](Reflection::FParams& Params) {
            Params.Set(L"WorldContextObject", reinterpret_cast<UObject*>(Pawn));
            Params.Set(L"PathStart", PawnLocation);
            Params.Set(L"GoalActor", GoalActor);
            Params.Set(L"TetherDistance", 50.0f);
            Params.Set(L"PathfindingContext", reinterpret_cast<AActor*>(Pawn));
        }))
    {
        bool bValid = false;
        bool bPartial = false;
        float Length = 0.0f;
        CallReflected(Path, L"IsValid", bValid);
        CallReflected(Path, L"IsPartial", bPartial);
        CallReflected(Path, L"GetPathLength", Length);
        PathText = !Path ? L"none" : !bValid ? L"invalid" : std::format(L"{} {:.0f}", bPartial ? L"partial" : L"complete", Length);
    }

    UWorld* World = Pawn->GetWorld();
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    std::wstring NavText = L"no navigation system";
    if (NavSys)
    {
        const TArray<ANavigationData*>& NavDataSet = NavSys->NavDataSet;
        NavText = std::format(L"{} navigation data", NavDataSet.Num());
        for (ANavigationData* NavData : NavDataSet)
        {
            NavText += L" " + Named(reinterpret_cast<UObject*>(NavData));
        }
        NavText += UNavigationSystem::IsNavigationBeingBuilt(reinterpret_cast<UObject*>(World)) ? L", being built" : L", built";
    }

    int32_t TimesDiscouraged = 0;
    FAIAssignmentInfo* AssignmentInfo = GetCurrent(AI) ? GetCurrent(AI)->GetAssignmentInfo(AI) : nullptr;
    if (AssignmentInfo)
    {
        TStructArray<FAIDiscouragedGoalTimer>& Timers = AssignmentInfo->GetDiscouragedGoalTimers();
        for (int32_t Index = 0; Index < Timers.Num(); ++Index)
        {
            if (Timers[Index].DiscouragedGoalInfo.Actor.Get() == reinterpret_cast<UObject*>(GoalActor))
            {
                TimesDiscouraged = static_cast<int32_t>(Timers[Index].NumberOfTimesMarkedForDiscouragement);
            }
        }
    }

    Log::Info(L"AFortAIGoalManager: {} ({}) {} goal {} ({} times in a row): visible {}, always perceived {}, distance {:.0f}; pawn speed {:.0f} of {:.0f}, move status {}; path {}; {}; discouraged {}",
        Named(reinterpret_cast<UObject*>(static_cast<AController*>(AI))), Named(reinterpret_cast<UObject*>(Pawn)->GetClass()), Reason, Named(reinterpret_cast<UObject*>(GoalActor)), Frustration.Count,
        BlackboardBool(Blackboard, L"GoalActorIsVisible"), BlackboardBool(Blackboard, L"GoalActorAlwaysPerceived"), GoalActor ? Pawn->GetDistanceTo(GoalActor) : -1.0f,
        Pawn->GetVelocity().Size(), MaxSpeed, static_cast<int32_t>(MoveStatus), PathText, NavText,
        AssignmentInfo ? std::format(L"{} times", TimesDiscouraged) : std::wstring(L"unavailable (no assignment info lookup)"));
#endif
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

bool AFortAIGoalManager::SetFrustrationDiscouragement(AFortAIController* AI, float DiscouragementDuration)
{
#if WITH_SERVER_CODE
    if (Natives::AFortAIGoalManager_SetFrustrationDiscouragement)
    {
        // 5.0+ keeps Epic's (it also passes the pawn's location to the world's UFortNavSystem first).
        return Natives::AFortAIGoalManager_SetFrustrationDiscouragement(this, AI, DiscouragementDuration);
    }

    FAIAssignmentInfo* AssignmentInfo = GetAssignmentInfo(AI);
    UWorld* World = AssignmentInfo ? GetWorld() : nullptr;
    if (!World)
    {
        return false;
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
            return true;
        }
    }

    FAIDiscouragedGoalTimer& Timer = Timers.AddDefaulted();
    FFortAIGoalInfo& DiscouragedGoalInfo = Timer.DiscouragedGoalInfo;
    std::memcpy(&DiscouragedGoalInfo, &CurrentGoal, FFortAIGoalInfo::StaticStruct()->GetStructureSize());
    Timer.ExpirationTime = CurrentTime + DiscouragementDuration;
    Timer.NumberOfTimesMarkedForDiscouragement = 1;
    return true;
#else
    return false;
#endif
}

void AFortAIGoalManager::SetCurrentGoalDiscouragement(UObject* WorldContext, AFortAIController* AI)
{
#if WITH_SERVER_CODE
    constexpr float DefaultDiscouragementDuration = 60.f;
    if (AFortAIGoalManager* GoalManager = GetCurrent(WorldContext))
    {
        GoalManager->SetFrustrationDiscouragement(AI, DefaultDiscouragementDuration);
        // GAB_Hunting's call: the hunt found no way to its goal (the pawn or the goal distance stopped changing).
        LogFrustratedAI(AI, L"gave up hunting");
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
