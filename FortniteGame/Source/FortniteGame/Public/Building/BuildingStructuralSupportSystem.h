#pragma once

#include "UObject/Object.h"

class ABuildingActor;
class AFortNavigationGraph;
class UClass;
struct FRotator;
struct FVector;
template <typename T>
class TArray;

// What the structural grid says about a building piece at a place (21.40's EFortStructuralGridQueryResults::Type, the
// same values from 3.0 to 5.40): CanAdd, or why not. ExistingActor: a piece already holds that grid slot.
namespace EFortStructuralGridQueryResults
{
    enum Type : uint8_t
    {
        CanAdd = 0,
        ExistingActor = 1,
        Obstructed = 2,
        NoStructuralSupport = 3,
        InvalidActor = 4,
        ReachedLimit = 5,
        NoEditPermission = 6,
        PatternNotPermittedByLayoutRequirement = 7,
        ResourceTypeNotPermittedByLayoutRequirement = 8,
        BuildingAtRequirementsDisabled = 9,
        BuildingOtherThanRequirementsDisabled = 10,
    };
}

class UBuildingStructuralSupportSystem : public UObject
{
    DefineUnrealClass(UBuildingStructuralSupportSystem, FortniteGame)

public:
    DefineUProperty(AFortNavigationGraph*, NavGraph)

    void InitializeGrid(const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ, int32_t MinCellZ);
    bool GetGrid(FVector& OutOrigin, int32_t OutSize[3]) const;

    // Whether a piece of BuildingSMActorClassToCheck may go at Location and Rotation: the grid's own query, which
    // AFortPlayerController::CanPlaceBuildableClassInStructuralGrid (5.40 0x1199920, after its permission and limit
    // checks) and AFortAIController::CreateBuildingActor (0xA28B40) ask before they spawn a piece, with
    // bAllowStaticOverlap false (5.40 0xD22670). OutExistingBuildings gets what the piece would replace (props a
    // player's piece destroys). Only where HasCanAddBuildingActorToGrid().
    EFortStructuralGridQueryResults::Type CanAddBuildingActorClassToGrid(UObject* WorldContextObject, UClass* BuildingSMActorClassToCheck, const FVector& Location, const FRotator& Rotation, bool bMirrored,
        TArray<ABuildingActor*>* OutExistingBuildings, bool bAllowStaticOverlap) const;
    // Whether the build reflects the grid's query: CanAddBuildingActorClassToGrid from 4.4, K2_CanAddBuildingActorToGrid
    // from 3.5. Before 3.5 the support system reflects neither (nor any other grid query).
    static bool HasCanAddBuildingActorToGrid();

    static void Hook();

private:
    static void InitializeGridHK(UBuildingStructuralSupportSystem* This, const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ, int32_t MinCellZ);
};
