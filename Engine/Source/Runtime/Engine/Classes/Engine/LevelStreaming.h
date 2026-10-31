#pragma once

#include "Math/Transform.h"
#include "UObject/Object.h"
#include "UObject/SoftObjectPtr.h"

class ULevel;
class UWorld;

class ULevelStreaming : public UObject
{
    DefineUnrealClass(ULevelStreaming, Engine)

public:
    DefineUProperty(TSoftObjectPtr<UWorld>, WorldAsset)
    DefineUProperty(FTransform, LevelTransform)
    DefineUProperty(ULevel*, LoadedLevel)

    FName GetWorldAssetPackageFName() const;
};
