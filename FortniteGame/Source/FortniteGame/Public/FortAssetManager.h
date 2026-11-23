#pragma once

#include "Engine/AssetManager.h"

class UFortGameData;

// 8.30 (CL-5793395): the parts of the game (EFortGameFeature) the asset manager has ready.
class EFortGameFeature
{
    DefineUnrealEnum(EFortGameFeature, FortniteGame)
    DefineUEnumValue(DedicatedServer)
};

class EFortGameFeatureState
{
    DefineUnrealEnum(EFortGameFeatureState, FortniteGame)
    DefineUEnumValue(Loaded)
};

class UFortAssetManager : public UAssetManager
{
    DefineUnrealClass(UFortAssetManager, FortniteGame)

public:
    DefineUProperty(UFortGameData*, GameData)

    static UFortAssetManager& Get();
    UFortGameData* GetGameData();

    // 8.30: UFortGameInstance::StartGameInstance (0x121B470) moves a dedicated server to the DedicatedServer feature,
    // which its check (0x11F8640) refuses until the asset manager has that feature available. The client build's
    // StartInitialLoading (0x11A4B30) readies only the Frontend (its server branch is compiled out), so the server
    // never browsed to its map: Core's StartInitialLoading readies DedicatedServer on the server.
    void StartInitialLoading();
    // The build's setter of a feature's state in FeatureInfos (8.30 0x11871F0, Core's name), which broadcasts it.
    void SetGameFeatureState(uint8_t Feature, uint8_t State, bool bBroadcast);
    static void Hook();

private:
    static inline void (*StartInitialLoadingOG)(UFortAssetManager* This) = nullptr;
    static void StartInitialLoadingHK(UFortAssetManager* This);
};
