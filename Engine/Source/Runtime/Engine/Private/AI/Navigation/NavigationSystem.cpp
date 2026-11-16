#include "pch.h"
#include "AI/Navigation/NavigationSystem.h"
#include "Natives.h"
#include "ReflectionParams.h"

void UNavigationSystem::RemoveNavigationBuildLock(uint8_t Flags)
{
    const bool bWasLocked = IsNavigationBuildingLocked();

    NavBuildingLockFlags = static_cast<uint8_t>(NavBuildingLockFlags & ~Flags);

    const bool bIsLocked = IsNavigationBuildingLocked();
    if (bWasLocked && !bIsLocked)
    {
        RebuildAll();
    }
}

void UNavigationSystem::ReleaseInitialBuildingLock()
{
    RemoveNavigationBuildLock(ENavigationBuildLock::InitialLock);
}

void UNavigationSystem::RebuildAll(bool bIsLoadTime)
{
    if (Natives::UNavigationSystem_RebuildAll)
    {
        Natives::UNavigationSystem_RebuildAll(this, bIsLoadTime);
    }
}

void UNavigationSystem::ConditionalPopulateNavOctree()
{
    if (Natives::UNavigationSystem_ConditionalPopulateNavOctree)
    {
        Natives::UNavigationSystem_ConditionalPopulateNavOctree(this);
    }
}

void UNavigationSystem::OnNavigationBoundsUpdated(ANavMeshBoundsVolume* NavVolume)
{
    Reflection::FParams Params(StaticFunction_OnNavigationBoundsUpdated());
    Params.Set(L"NavVolume", reinterpret_cast<UObject*>(NavVolume));
    Params.Call(this);
}

void UNavigationSystem::RequestRegistration(ANavigationData* NavData, bool bTriggerRegistrationProcessing)
{
    if (Natives::UNavigationSystem_RequestRegistration)
    {
        Natives::UNavigationSystem_RequestRegistration(this, NavData, bTriggerRegistrationProcessing);
    }
}

bool UNavigationSystem::CanSpawnMissingNavigationData()
{
    return Natives::UNavigationSystem_SpawnMissingNavigationDataIndex >= 0
        && (Natives::UNavigationSystem_ProcessRegistrationCandidatesIndex >= 0 || Natives::UNavigationSystem_ProcessRegistrationCandidates);
}

void UNavigationSystem::SpawnMissingNavigationData()
{
    if (Natives::UNavigationSystem_SpawnMissingNavigationDataIndex >= 0)
    {
        using FSpawnMissingNavigationData = void (*)(UNavigationSystem* This);
        void** VTable = *reinterpret_cast<void***>(this);
        reinterpret_cast<FSpawnMissingNavigationData>(VTable[Natives::UNavigationSystem_SpawnMissingNavigationDataIndex])(this);
    }
}

void UNavigationSystem::ProcessRegistrationCandidates()
{
    if (Natives::UNavigationSystem_ProcessRegistrationCandidatesIndex >= 0)
    {
        using FProcessRegistrationCandidates = void (*)(UNavigationSystem* This);
        void** VTable = *reinterpret_cast<void***>(this);
        reinterpret_cast<FProcessRegistrationCandidates>(VTable[Natives::UNavigationSystem_ProcessRegistrationCandidatesIndex])(this);
    }
    else if (Natives::UNavigationSystem_ProcessRegistrationCandidates)
    {
        Natives::UNavigationSystem_ProcessRegistrationCandidates(this);
    }
}

bool UNavigationSystem::InitializeForWorld(UWorld* World, FNavigationSystemRunMode::Type Mode)
{
    if (!Natives::UNavigationSystem_InitializeForWorld || !World)
    {
        return false;
    }
    Natives::UNavigationSystem_InitializeForWorld(World, Mode);
    return true;
}

bool FNavigationSystem::AddNavigationSystemToWorld(UWorld& WorldOwner, FNavigationSystemRunMode::Type RunMode, bool bInitializeForWorld)
{
    if (!Natives::FNavigationSystem_AddNavigationSystemToWorld)
    {
        return false;
    }
    if (Natives::FNavigationSystem_AddNavigationSystemToWorldTakesConfig)
    {
        // 6.00's takes the config third; null uses the world settings' own.
        using FAddWithConfig = void (*)(UWorld& WorldOwner, uint8_t RunMode, void* NavigationSystemConfig, bool bInitializeForWorld);
        reinterpret_cast<FAddWithConfig>(Natives::FNavigationSystem_AddNavigationSystemToWorld)(WorldOwner, RunMode, nullptr, bInitializeForWorld);
        return true;
    }
    Natives::FNavigationSystem_AddNavigationSystemToWorld(WorldOwner, RunMode, bInitializeForWorld);
    return true;
}

bool UNavigationSystem::IsNavigationBeingBuilt(UObject* WorldContextObject)
{
    Reflection::FParams Params(StaticFunction_IsNavigationBeingBuilt());
    Params.Set(Params.Has(L"WorldContextObject") ? L"WorldContextObject" : L"WorldContext", WorldContextObject);
    return Params.Call(StaticClass()->GetDefaultObject()) && Params.Take<bool>(L"ReturnValue");
}

void UNavigationSystem::RegisterNavigationInvoker(AActor* Invoker, float TileGenerationRadius, float TileRemovalRadius)
{
    Reflection::FParams Params(StaticFunction_RegisterNavigationInvoker(), true);
    Params.Set(L"Invoker", reinterpret_cast<UObject*>(Invoker));
    Params.Set(L"TileGenerationRadius", TileGenerationRadius);
    Params.Set(L"TileRemovalRadius", TileRemovalRadius);
    Params.Call(this);
}

bool UNavigationSystem::K2_ProjectPointToNavigation(UObject* WorldContextObject, const FVector& Point, FVector& ProjectedLocation, ANavigationData* NavData, const FVector& QueryExtent)
{
    // UE 4.19 (1.10) added it: builds before 1.10 have only the older ProjectPointToNavigation (below), and asking for this
    // one there stopped a 1.8.2 server at the bots' first path request. False, so the caller takes the older one.
    UFunction* Function = FindStaticFunction_K2_ProjectPointToNavigation();
    if (!Function)
    {
        return false;
    }
    Reflection::FParams Params(Function, true);
    Params.Set(L"WorldContextObject", WorldContextObject);
    Params.Set(L"Point", Point);
    Params.Set(L"NavData", reinterpret_cast<UObject*>(NavData));
    Params.Set(L"QueryExtent", QueryExtent);
    if (!Params.Call(StaticClass()->GetDefaultObject()) || !Params.Take<bool>(L"ReturnValue"))
    {
        return false;
    }
    ProjectedLocation = Params.Take<FVector>(L"ProjectedLocation");
    return true;
}

FVector UNavigationSystem::ProjectPointToNavigation(UObject* WorldContextObject, const FVector& Point, ANavigationData* NavData, const FVector& QueryExtent)
{
    // UE 4.16's body: FNavLocation ProjectedPoint(Point), the system's ProjectPointToNavigation on NavData (the main data
    // when none is given) within QueryExtent (INVALID_NAVEXTENT when it is zero) and the filter class's query filter, then
    // ProjectedPoint.Location: Point itself when the projection found nothing. The filter class is left at none.
    UFunction* Function = FindStaticFunction_ProjectPointToNavigation();
    if (!Function)
    {
        return Point;
    }
    // UE 4.16 names its world context parameter WorldContext (1.7.2's reflection, 0x2A57150, and 1.8's, 0x2AD4610: Point,
    // NavData, FilterClass, QueryExtent, WorldContext), as IsNavigationBeingBuilt does on those builds. Set as
    // WorldContextObject, it stayed null, and the world context lookup called GetWorld on it (1.8 0x13DB060,
    // "mov rax, [rcx]"): a 1.8 server died as its first bot landed (CoreV2 #473). With neither name it isn't called.
    Reflection::FParams Params(Function, true);
    if (!Params.Set(Params.Has(L"WorldContextObject") ? L"WorldContextObject" : L"WorldContext", WorldContextObject))
    {
        return Point;
    }
    Params.Set(L"Point", Point);
    Params.Set(L"NavData", reinterpret_cast<UObject*>(NavData));
    Params.Set(L"QueryExtent", QueryExtent);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<FVector>(L"ReturnValue") : Point;
}
