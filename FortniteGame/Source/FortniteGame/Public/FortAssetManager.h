#pragma once

#include "Engine/AssetManager.h"

class UFortGameData;

class UFortAssetManager : public UAssetManager
{
    DefineUnrealClass(UFortAssetManager, FortniteGame)

public:
    DefineUProperty(UFortGameData*, GameData)

    static UFortAssetManager& Get();
    UFortGameData* GetGameData();
};
