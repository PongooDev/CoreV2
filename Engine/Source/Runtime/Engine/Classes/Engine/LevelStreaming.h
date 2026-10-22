#pragma once

#include "Math/Transform.h"
#include "UObject/Object.h"

class ULevel;

class ULevelStreaming : public UObject
{
    DefineUnrealClass(ULevelStreaming, Engine)

public:
    DefineUProperty(FTransform, LevelTransform)
    DefineUProperty(ULevel*, LoadedLevel)
};
