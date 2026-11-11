#include "pch.h"
#include "Building/BuildingStructuralSupportSystem.h"
#include "AI/Navigation/FortNavigationGraph.h"
#include "Building/BuildingSMActor.h"
#include "Containers/Array.h"
#include "Engine/World.h"
#include "Hooks.h"
#include "Math/Rotator.h"
#include "Math/Vector.h"
#include "Natives.h"
#include "ReflectionParams.h"

namespace
{
    // 5.0+ added MinCellZ (the grid origin's Z is 384 * MinCellZ; 1.1-4.5 hard-code -3840). It is passed on every build:
    // the six-argument function never reads the seventh slot.
    void (*InitializeGridOG)(UBuildingStructuralSupportSystem* This, const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ, int32_t MinCellZ) = nullptr;

    // The grid's query by class (4.4+), and by actor (3.5+). Neither is on every build, so they are looked up, not declared.
    UFunction* CanAddBuildingActorClassToGridFunction()
    {
        static UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.BuildingStructuralSupportSystem:CanAddBuildingActorClassToGrid");
        return Function;
    }

    UFunction* K2CanAddBuildingActorToGridFunction()
    {
        static UFunction* Function = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.BuildingStructuralSupportSystem:K2_CanAddBuildingActorToGrid");
        return Function;
    }

    // ABuildingActor::GetStructuralGridWorldLocation (5.40 vtable slot 288, 0xCC5FD0): where a piece with its pivot at
    // Location and Rotation is on the grid, the pivot moved back by its BaseLocToPivotOffset turned with it, unless
    // bRotateInPlaceGame. ABuildingSMActor's constructor sets the offset half a cell along -Y (0xC761E0) and
    // ABuildingWall's sets bRotateInPlaceGame (0xCE7FC0), so a ramp's or a cone's place is its cell's middle and a
    // wall's its edge's. A native virtual whose slot moves between builds, so its body is followed with the building's
    // properties (both there from 3.0).
    FVector GetStructuralGridWorldLocation(const UObject* Building, const FVector& Location, const FRotator& Rotation)
    {
        static UProperty* OffsetProperty = ABuildingActor::StaticClass()->FindPropertyByName(FName(L"BaseLocToPivotOffset", FNAME_Find));
        static UBoolProperty* RotateInPlaceProperty = static_cast<UBoolProperty*>(ABuildingActor::StaticClass()->FindPropertyByName(FName(L"bRotateInPlaceGame", FNAME_Find)));
        if (!OffsetProperty || static_cast<size_t>(OffsetProperty->ElementSize) != sizeof(FVector) || (RotateInPlaceProperty && RotateInPlaceProperty->GetPropertyValue_InContainer(Building)))
        {
            return Location;
        }
        return Location - Rotation.Quaternion().RotateVector(*OffsetProperty->ContainerPtrToValuePtr<FVector>(Building));
    }
}

bool UBuildingStructuralSupportSystem::HasCanAddBuildingActorToGrid()
{
    return CanAddBuildingActorClassToGridFunction() || K2CanAddBuildingActorToGridFunction();
}

EFortStructuralGridQueryResults::Type UBuildingStructuralSupportSystem::CanAddBuildingActorClassToGrid(UObject* WorldContextObject, UClass* BuildingSMActorClassToCheck, const FVector& Location,
    const FRotator& Rotation, bool bMirrored, TArray<ABuildingActor*>* OutExistingBuildings, bool bAllowStaticOverlap) const
{
    // 5.40's (0xD22670): the class's default object, a building SM actor's, asked through CanAddBuildingActorToGrid
    // (0xD22780) at its GetStructuralGridWorldLocation for Location and Rotation. 3.5 to 4.2 reflect only that query by
    // actor (K2_CanAddBuildingActorToGrid, CanAddBuildingActorToGrid itself, 5.40 0x1388950), so the default object is
    // asked there at the place on the grid the 4.4+ function works out.
    UObject* DefaultObject = BuildingSMActorClassToCheck && BuildingSMActorClassToCheck->IsChildOf(ABuildingSMActor::StaticClass()) ? BuildingSMActorClassToCheck->GetDefaultObject() : nullptr;
    UFunction* ByClass = CanAddBuildingActorClassToGridFunction();
    UFunction* Function = ByClass ? ByClass : K2CanAddBuildingActorToGridFunction();
    if (!DefaultObject || !Function)
    {
        return EFortStructuralGridQueryResults::InvalidActor;
    }
    Reflection::FParams Params(Function, true);
    uint8_t* Result = Params.Get<uint8_t>(L"ReturnValue");
    const bool bSet = Result && Params.Set(L"WorldContextObject", WorldContextObject)
        && (ByClass ? Params.Set(L"BuildingSMActorClassToCheck", BuildingSMActorClassToCheck) : Params.Set(L"ActorToCheck", DefaultObject))
        && Params.Set(L"Location", ByClass ? Location : GetStructuralGridWorldLocation(DefaultObject, Location, Rotation)) && Params.Set(L"Rotation", Rotation) && Params.Set(L"bMirrored", bMirrored)
        && Params.Set(L"bAllowStaticOverlap", bAllowStaticOverlap);
    if (!bSet || !Params.Call(const_cast<UBuildingStructuralSupportSystem*>(this)))
    {
        return EFortStructuralGridQueryResults::InvalidActor;
    }
    if (OutExistingBuildings)
    {
        *OutExistingBuildings = Params.Take<TArray<ABuildingActor*>>(ByClass ? L"OutExistingBuildings" : L"ExistingBuildings");
    }
    return static_cast<EFortStructuralGridQueryResults::Type>(*Result);
}

bool UBuildingStructuralSupportSystem::GetGrid(FVector& OutOrigin, int32_t OutSize[3]) const
{
    if (Natives::UBuildingStructuralSupportSystem_GridSize < 0 || Natives::UBuildingStructuralSupportSystem_GridOrigin < 0)
    {
        return false;
    }
    const uint8_t* Base = reinterpret_cast<const uint8_t*>(this);
    const int32_t* Size = reinterpret_cast<const int32_t*>(Base + Natives::UBuildingStructuralSupportSystem_GridSize);
    OutOrigin = *reinterpret_cast<const FVector*>(Base + Natives::UBuildingStructuralSupportSystem_GridOrigin);
    for (int32_t Axis = 0; Axis < 3; ++Axis)
    {
        OutSize[Axis] = Size[Axis];
    }
    return OutSize[0] > 0 && OutSize[1] > 0 && OutSize[2] > 0;
}

void UBuildingStructuralSupportSystem::InitializeGrid(const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ, int32_t MinCellZ)
{
    InitializeGridOG(this, GridCenter, TileCells, TilesX, TilesY, CellsZ, MinCellZ);

#if WITH_SERVER_CODE
    FVector GridOrigin;
    int32_t GridSize[3] = {};
    if (!NavGraph && GetGrid(GridOrigin, GridSize))
    {
        UWorld* World = GetWorld();
        if (AFortNavigationGraph* Graph = AFortNavigationGraph::FindOrSpawn(World ? World : static_cast<UWorld*>(GWorld)))
        {
            Graph->InitializeCells(this);
        }
    }
#endif
}

void UBuildingStructuralSupportSystem::Hook()
{
    if (!AFortNavigationGraph::ResolveNatives())
    {
        return;
    }
    Hooks::Detour(L"UBuildingStructuralSupportSystem::InitializeGrid", reinterpret_cast<void*>(Natives::UBuildingStructuralSupportSystem_InitializeGrid), &InitializeGridHK, &InitializeGridOG);
}

void UBuildingStructuralSupportSystem::InitializeGridHK(UBuildingStructuralSupportSystem* This, const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ, int32_t MinCellZ)
{
    This->InitializeGrid(GridCenter, TileCells, TilesX, TilesY, CellsZ, MinCellZ);
}
