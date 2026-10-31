#pragma once

#include "Math/Transform.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class ULevel;
class UWorld;

class ULevelStreaming : public UObject
{
    DefineUnrealClass(ULevelStreaming, Engine)

public:
    DefineUProperty(TAssetPtr<UWorld>, WorldAsset)
    DefineUProperty(FTransform, LevelTransform)
    DefineUProperty(ULevel*, LoadedLevel)

    FName GetWorldAssetPackageFName() const;
};
