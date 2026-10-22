#pragma once

#include "UObject/Object.h"

class AFortNavigationGraph;
struct FVector;

class UBuildingStructuralSupportSystem : public UObject
{
    DefineUnrealClass(UBuildingStructuralSupportSystem, FortniteGame)

public:
    DefineUProperty(AFortNavigationGraph*, NavGraph)

    void InitializeGrid(const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ);
    bool GetGrid(FVector& OutOrigin, int32_t OutSize[3]) const;

    static void Hook();

private:
    static void InitializeGridHK(UBuildingStructuralSupportSystem* This, const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ);
};
