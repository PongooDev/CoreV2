#include "pch.h"
#include "AI/FortAIGoalManager.h"
#include "AI/FortAIDirector.h"
#include "AI/FortQueryContexts.h"
#include "AI/Navigation/FortNavigationGraph.h"
#include "AI/Navigation/NavigationSystem.h"
#include "AI/Navigation/RecastNavMesh.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "Finder.h"
#include "FortGameModeZone.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortAIPawn.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UnrealType.h"
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

    // An object pointer member of Object, by its reflected name (a property Core has no wrapper for).
    UObject* ObjectProperty(UObject* Object, const wchar_t* Name)
    {
        UProperty* Property = Object ? Object->GetClass()->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property && Property->ElementSize == sizeof(UObject*) ? *reinterpret_cast<UObject**>(reinterpret_cast<uint8_t*>(Object) + Property->GetOffset_ForInternal()) : nullptr;
    }

    // Whether NavData has anything at Point: K2_ProjectPointToNavigation within the data's default query extent (the zero
    // extent the parameters start with is INVALID_NAVEXTENT).
    bool ProjectsToNavigation(APawn* Pawn, const FVector& Point, ANavigationData* NavData)
    {
        bool bProjected = false;
        CallReflected(UNavigationSystem::StaticClass()->GetDefaultObject(), L"K2_ProjectPointToNavigation", bProjected, [&](Reflection::FParams& Params) {
            Params.Set(Params.Has(L"WorldContextObject") ? L"WorldContextObject" : L"WorldContext", reinterpret_cast<UObject*>(Pawn));
            Params.Set(L"Point", Point);
            Params.Set(L"NavData", NavData);
        });
        return bProjected;
    }

    // Core's diagnostics (#408): the step of the pawn's way to its goal that fails.
    // - The navigation data. The pawn's is its path following component's MyNavData (UNavigationSystem::GetNavDataForProps
    //   for its agent: what its move requests and EQS_Hunting's grid projection and pathfinding test use), else the
    //   system's main one. A husk's is FortNavMesh-Husk. Without the navmeshes GetNavDataForProps gives it the
    //   best-fitting agent's data, the graph, or none: no navmesh to walk on. That is reported as the failure, with the
    //   graph's cells under both ends (an empty graph region shows there).
    // - Projecting the start, then the goal, onto the navmesh: no tile there yet, or off the navmesh.
    // - The search itself (FindPathToLocationSynchronously on that navigation data). The engine's synchronous search
    //   returns no path object, before searching at all, while the system has no main navigation data.
    std::wstring DescribeHuntingPath(AFortAIController* AI, APawn* Pawn, AActor* GoalActor, const FVector& PawnLocation, UNavigationSystem* NavSys)
    {
        if (!GoalActor)
        {
            return L"no goal actor";
        }
        if (!NavSys)
        {
            return L"fails at the navigation system (the world has none)";
        }

        UObject* PathFollowing = nullptr;
        CallReflected(AI, L"GetPathFollowingComponent", PathFollowing);
        ANavigationData* AgentNavData = reinterpret_cast<ANavigationData*>(ObjectProperty(PathFollowing, L"MyNavData"));
        ANavigationData* MainNavData = NavSys->GetDefaultNavDataInstance();
        ANavigationData* NavData = AgentNavData ? AgentNavData : MainNavData;
        FVector GoalLocation(0, 0, 0);
        CallReflected(GoalActor, L"K2_GetActorLocation", GoalLocation);

        AFortNavigationGraph* Graph = AFortNavigationGraph::GetCurrent(Pawn->GetWorld());
        bool bStartBuilt = false;
        bool bGoalBuilt = false;
        const std::wstring GraphText = Graph ? std::format(L"graph cells: start {}, goal {}", Graph->DescribeCellAt(PawnLocation, bStartBuilt), Graph->DescribeCellAt(GoalLocation, bGoalBuilt)) : std::wstring(L"no graph");
        const std::wstring DataText = std::format(L"the agent's navigation data {}, main {}", Named(reinterpret_cast<UObject*>(AgentNavData)), Named(reinterpret_cast<UObject*>(MainNavData)));

        if (!NavData)
        {
            return std::format(L"fails at the navigation data (none for the pawn's agent, no main one); {}", GraphText);
        }
        if (!reinterpret_cast<UObject*>(NavData)->IsA(ARecastNavMesh::StaticClass()))
        {
            // Nothing is asked of it: the graph has no FindPathImplementation (6.00's constructor, 0xB0B170, sets none),
            // so a path request on it calls a null pointer.
            return std::format(L"fails at {}; {}; {}", Graph && (!bStartBuilt || !bGoalBuilt) ? L"an empty graph region, and the pawn's navigation data is no navmesh (none for its agent)"
                : L"the navigation data (no navmesh for the pawn's agent)", DataText, GraphText);
        }

        const bool bStartProjected = ProjectsToNavigation(Pawn, PawnLocation, NavData);
        const bool bGoalProjected = ProjectsToNavigation(Pawn, GoalLocation, NavData);

        std::wstring SearchText = L"not run";
        bool bFound = false;
        UObject* Path = nullptr;
        if (MainNavData && CallReflected(UNavigationSystem::StaticClass()->GetDefaultObject(), L"FindPathToLocationSynchronously", Path, [&](Reflection::FParams& Params) {
                Params.Set(Params.Has(L"WorldContextObject") ? L"WorldContextObject" : L"WorldContext", reinterpret_cast<UObject*>(Pawn));
                Params.Set(L"PathStart", PawnLocation);
                Params.Set(L"PathEnd", GoalLocation);
                Params.Set(L"PathfindingContext", reinterpret_cast<AActor*>(NavData));
            }))
        {
            bool bValid = false;
            bool bPartial = false;
            float Length = 0.0f;
            CallReflected(Path, L"IsValid", bValid);
            CallReflected(Path, L"IsPartial", bPartial);
            CallReflected(Path, L"GetPathLength", Length);
            bFound = Path && bValid;
            SearchText = !Path ? L"no path object" : !bValid ? L"no path" : std::format(L"{} {:.0f}", bPartial ? L"partial" : L"complete", Length);
        }

        const wchar_t* Failure = !bStartProjected ? L"projecting the start"
            : !bGoalProjected ? L"projecting the goal"
            : !MainNavData ? L"the search (not run: the system has no main navigation data)"
            : !bFound ? L"the search"
            : L"nothing (a path exists)";
        return std::format(L"fails at {}; {}, start projected {}, goal projected {}, search {}; {}", Failure, DataText, bStartProjected ? L"yes" : L"no", bGoalProjected ? L"yes" : L"no", SearchText, GraphText);
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

    UWorld* World = Pawn->GetWorld();
    UNavigationSystem* NavSys = World ? World->GetNavigationSystem() : nullptr;
    const std::wstring PathText = DescribeHuntingPath(AI, Pawn, GoalActor, PawnLocation, NavSys);
    std::wstring NavText = L"no navigation system";
    if (NavSys)
    {
        const TArray<ANavigationData*>& NavDataSet = NavSys->NavDataSet;
        NavText = std::format(L"{} navigation data", NavDataSet.Num());
        for (ANavigationData* NavData : NavDataSet)
        {
            NavText += L" " + Named(reinterpret_cast<UObject*>(NavData));
        }
        // The build lock, not UNavigationSystem::IsNavigationBeingBuilt: that one crashed the server when it was asked
        // while a zone's navmesh was building (8f07680), which is when a husk gives up now that the navmeshes exist.
        if (Natives::UNavigationSystem_NavBuildingLockFlags >= 0)
        {
            NavText += std::format(L", build lock 0x{:X}", static_cast<int32_t>(NavSys->NavBuildingLockFlags));
        }
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

    Log::Info(L"AFortAIGoalManager: {} ({}) {} goal {} ({} times in a row): visible {}, always perceived {}, distance {:.0f}; pawn speed {:.0f} of {:.0f}, move status {}; path: {}; {}; discouraged {}",
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

namespace
{
    // IFortAIEncounterInfoOwnerInterface::GetEncounterInfo on Pawn, as the goal manager asks an AFortAIPawn for its
    // encounter (6.20 CL-4497486 0xAC6B20 calls the interface's slot 2 at pawn +0x780).
    UFortAIEncounterInfo* GetEncounterInfoOf(AFortAIPawn* Pawn)
    {
        if (!Pawn || !Natives::UObject_GetInterfaceAddress || Natives::IFortAIEncounterInfoOwnerInterface_GetEncounterInfoIndex < 0)
        {
            return nullptr;
        }
        void* EncounterInfoOwner = Natives::UObject_GetInterfaceAddress(reinterpret_cast<UObject*>(Pawn), UFortAIEncounterInfoOwnerInterface::StaticClass());
        return EncounterInfoOwner ? CallVirtual<UFortAIEncounterInfo*>(EncounterInfoOwner, Natives::IFortAIEncounterInfoOwnerInterface_GetEncounterInfoIndex) : nullptr;
    }

    AFortAIController* AIControllerOf(AFortAIPawn* Pawn)
    {
        return Pawn ? Cast<AFortAIController>(reinterpret_cast<UObject*>(static_cast<AController*>(Pawn->Controller))) : nullptr;
    }

    UObject* AsObject(AFortAIController* AI)
    {
        return reinterpret_cast<UObject*>(static_cast<AController*>(AI));
    }
}

void AFortAIGoalManager::AddValidGoalsForAIToEQI(AFortAIPawn* Pawn, void* QueryInstance)
{
#if WITH_SERVER_CODE
    if (!Pawn)
    {
        return;
    }
    // 6.20 copies the encounter's assignments before it walks them; nothing on the way changes them.
    if (UFortAIEncounterInfo* EncounterInfo = GetEncounterInfoOf(Pawn))
    {
        AddValidGoalsForAIToEQI(Pawn, EncounterInfo->EncounterAssignments, QueryInstance);
    }
    AddValidGoalsForAIToEQI(Pawn, WorldAssignments, QueryInstance);
#endif
}

void AFortAIGoalManager::AddValidGoalsForAIToEQI(AFortAIPawn* Pawn, const TArray<UObject*>& Assignments, void* QueryInstance)
{
#if WITH_SERVER_CODE
    AFortAIController* AI = AIControllerOf(Pawn);
    if (!AI || !Natives::UFortAIAssignment_CanPawnUse)
    {
        return;
    }
    for (UObject* Assignment : Assignments)
    {
        if (Assignment && Natives::UFortAIAssignment_CanPawnUse(Assignment, Pawn))
        {
            static_cast<UFortAIAssignment*>(Assignment)->AddValidGoalsForAIToEQI(AI, Pawn, QueryInstance);
        }
    }
#endif
}

void AFortAIGoalManager::AddValidGoalsForAI(AFortAIPawn* Pawn, TArray<FFortAIAssignmentGoalInfo>& OutGoals)
{
#if WITH_SERVER_CODE
    if (!Pawn)
    {
        return;
    }
    if (UFortAIEncounterInfo* EncounterInfo = GetEncounterInfoOf(Pawn))
    {
        AddValidGoalsForAI(Pawn, EncounterInfo->EncounterAssignments, OutGoals);
    }
    AddValidGoalsForAI(Pawn, WorldAssignments, OutGoals);
#endif
}

void AFortAIGoalManager::AddValidGoalsForAI(AFortAIPawn* Pawn, const TArray<UObject*>& Assignments, TArray<FFortAIAssignmentGoalInfo>& OutGoals)
{
#if WITH_SERVER_CODE
    AFortAIController* AI = AIControllerOf(Pawn);
    if (!AI || !Natives::UFortAIAssignment_CanPawnUse)
    {
        return;
    }
    for (UObject* Assignment : Assignments)
    {
        if (Assignment && Natives::UFortAIAssignment_CanPawnUse(Assignment, Pawn))
        {
            static_cast<UFortAIAssignment*>(Assignment)->AddValidGoalsForAI(AI, Pawn, OutGoals);
        }
    }
#endif
}

void AFortAIGoalManager::SuppressGoalUpdates(AFortAIController* AI, bool bSuppress)
{
#if WITH_SERVER_CODE
    FAIAssignmentInfo* AssignmentInfo = GetAssignmentInfo(AI);
    if (!AssignmentInfo || Natives::FAIAssignmentInfo_LastQueryResult < 0)
    {
        return;
    }
    if (!bSuppress && AssignmentInfo->bSuppressGoalUpdates && AssignmentInfo->LastQueryResult.IsValid())
    {
        const TSharedPtr<FEnvQueryResult> LastQueryResult = AssignmentInfo->LastQueryResult;
        const bool bHasPawn = AFortAIController::HasMyFortPawn() ? AI->MyFortPawn != nullptr : AI->GetPawn() != nullptr;
        if (bHasPawn && Natives::AFortAIGoalManager_HandleGoalQueryResult)
        {
            Natives::AFortAIGoalManager_HandleGoalQueryResult(this, AsObject(AI), AssignmentInfo, &LastQueryResult);
        }
    }
    AssignmentInfo->bSuppressGoalUpdates = bSuppress;
    if (!bSuppress)
    {
        AssignmentInfo->LastQueryResult = TSharedPtr<FEnvQueryResult>();
    }
#endif
}

void AFortAIGoalManager::RegisterAIForGoalDetection(AFortAIController* AI)
{
    if (AI && Natives::AFortAIGoalManager_RegisterAIForGoalDetection)
    {
        Natives::AFortAIGoalManager_RegisterAIForGoalDetection(this, AsObject(AI));
    }
}

void AFortAIGoalManager::UnregisterAIForGoalDetection(AFortAIController* AI)
{
    if (AI && Natives::AFortAIGoalManager_UnregisterAIForGoalDetection)
    {
        Natives::AFortAIGoalManager_UnregisterAIForGoalDetection(this, AsObject(AI));
    }
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
        if (Hooks::RedirectCall(L"AFortAIGoalManager::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrent)))
        {
            Natives::AFortAIGoalManager_GetCurrentCalls.push_back(Call);
        }
    }
    // 5.0+: the encounter's primary assignment lookup (FindAssignment inlined), which its spawn queries' contexts use.
    for (uint8_t* Call : EncounterCalls)
    {
        if (Hooks::RedirectCall(L"AFortAIGoalManager::GetCurrent (encounter assignments)", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrentForEncounterAssignments)))
        {
            Natives::AFortAIGoalManager_GetCurrentCalls.push_back(Call);
        }
    }
    // What tells a goal manager user the compiler kept apart from one it folded past GetCurrent (Finder::IsFoldedGoalManagerUser).
    Natives::AFortAIGoalManager_GetCurrentStub = Stub;
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
        uint8_t* Call = Stub && Function ? Finder::FirstCallInImplementation(Function, Stub) : nullptr;
        if (Call && Hooks::RedirectCall(L"AFortAIGoalManager::GetCurrent", Call, reinterpret_cast<void*>(&AFortAIGoalManager::GetCurrent)))
        {
            Natives::AFortAIGoalManager_GetCurrentCalls.push_back(Call);
        }
    }

    Finder::ResolveGoalManagerLive();
}
