#pragma once

#include "Building/BuildingSMActor.h"
#include "Math/Box.h"
#include "Reflection.h"

// A building's or a POI's plot: its static mesh is the plot, and the level it streams in (a building or POI sublevel,
// /Temp/Game/... once loaded) sits on it.
class ABuildingFoundation : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingFoundation, FortniteGame)

public:
    // The streamed level's package (/Temp/Game/Athena/Maps/POI/Athena_POI_City_001_43); None until it streams.
    DefineUProperty(FName, LevelToStream)
    // The plot's box in the foundation's space, measured from the streamed level's foundations (editor-built).
    DefineUProperty(FBox, FoundationBasedBoundingBox)
    DefineUProperty(FBox, StreamingBoundingBox)
};
