#pragma once

#include "Math/Transform.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"
#include "UObject/ScriptDelegates.h"

class ULevel;
class UWorld;

class ULevelStreaming : public UObject
{
    DefineUnrealClass(ULevelStreaming, Engine)

public:
    DefineUProperty(TAssetPtr<UWorld>, WorldAsset)
    DefineUProperty(FTransform, LevelTransform)
    DefineUProperty(ULevel*, LoadedLevel)
    // Broadcast once the level is made visible (its actors have begun play).
    DefineUProperty(FMulticastScriptDelegate, OnLevelShown)

    FName GetWorldAssetPackageFName() const;
};
