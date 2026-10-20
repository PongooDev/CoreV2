#include "pch.h"
#include "FortAssetManager.h"
#include "Engine/Engine.h"
#include "Logging/LogMacros.h"
#include "Templates/Casts.h"

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

UFortAssetManager& UFortAssetManager::Get()
{
    UFortAssetManager* Singleton = Cast<UFortAssetManager>(GEngine->AssetManager);
    if (!Singleton)
    {
        UE_LOG(LogFort, Fatal, TEXT("Invalid Engine class used, must be FortEngine or FortUnrealEdEngine"));
    }
    return *Singleton;
}

UFortGameData* UFortAssetManager::GetGameData()
{
    return GameData;
}
