#pragma once

#include "Containers/Array.h"
#include "GameFramework/WorldSettings.h"
#include "Math/Vector.h"

class AFortWorldSettings : public AWorldSettings
{
    DefineUnrealClass(AFortWorldSettings, FortniteGame)

public:
    DefineUProperty(TArray<int32_t>, WorldCells)
    DefineUProperty(FVector, WorldCellsOrigin)
};
