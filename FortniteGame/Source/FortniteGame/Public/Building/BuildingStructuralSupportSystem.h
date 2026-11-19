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

// How the grid moved a piece's build preview marker (7.00's reflected values, CL-4629139): the grid's query gives it back
// from 7.00 on (CanAddBuildingActorClassToGrid's MarkerOptionalAdjustment out parameter, which
// AFortPlayerController::CanPlaceBuildableClassInStructuralGrid takes too).
enum class EFortBuildPreviewMarkerOptionalAdjustment : uint8_t
{
    None = 0,
    FreeWallPieceOnTop = 1,
    FreeWallPieceOnBottom = 2,
};

// A grid cell (FBuildingSupportCellIndex): its X, Y and Z indices.
struct FBuildingSupportCellIndex
{
    int32_t X = 0;
    int32_t Y = 0;
    int32_t Z = 0;
};
static_assert(sizeof(FBuildingSupportCellIndex) == 0xC);

class UBuildingStructuralSupportSystem : public UObject
{
    DefineUnrealClass(UBuildingStructuralSupportSystem, FortniteGame)

public:
    DefineUProperty(AFortNavigationGraph*, NavGraph)

    void InitializeGrid(const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ, int32_t MinCellZ);
    bool GetGrid(FVector& OutOrigin, int32_t OutSize[3]) const;
    // GridBounds (21.40's name, an FBox right after GridOrigin): the box InitializeGrid lays the grid out in, half a cell
    // past its outer cells' centres in X and Y. The navigation graph's OnWorldInitialized copies it as its own bounds
    // (6.22 0xB5A686, 6.30 0xB7F0E6, 6.31 0xB80B66: movups xmm0, [support system+GridBounds]), which is what the offset
    // is checked against. False where it isn't found or the grid isn't laid out.
    bool GetGridBounds(struct FBox& OutBounds) const;
    // The grid cell at WorldLocation (reflected from 3.5 on; 6.31's exec 0x14C6300 -> 0xE05010): X and Y rounded to the
    // nearest cell centre, GridOrigin plus whole 512 uu cells, so a cell's edges are half a cell off the grid origin's; Z
    // floored to a 384 uu level above GridOrigin. The box is the cell (centre +-256 in X and Y, the level in Z). Its
    // IsValid is false where the build doesn't reflect it or the call fails.
    struct FBox K2_GetGridBoxFromWorldLoc(const FVector& WorldLocation) const;
    // The grid's cell at WorldLoc, and a cell's world location (both reflected from 3.5). False where the build doesn't
    // reflect them or the grid says the place is off it.
    bool K2_GetGridIndicesFromWorldLoc(const FVector& WorldLoc, FBuildingSupportCellIndex& OutGridIndices);
    bool K2_GetWorldLocFromGridIndices(const FBuildingSupportCellIndex& GridIndices, FVector& OutWorldLoc) const;

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
