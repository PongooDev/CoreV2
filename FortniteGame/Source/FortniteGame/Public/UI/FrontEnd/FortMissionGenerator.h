#pragma once

#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class UFortMissionInfo;

class UFortMissionGenerator : public UObject
{
    DefineUnrealClass(UFortMissionGenerator, FortniteGame)

public:
    DefineUProperty(TAssetPtr<UFortMissionInfo>, PrimaryMissionInfo)
};
