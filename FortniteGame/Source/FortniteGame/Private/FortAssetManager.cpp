#include "pch.h"
#include "FortAssetManager.h"
#include "Engine/Engine.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Templates/Casts.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"

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

void UFortAssetManager::SetGameFeatureState(uint8_t Feature, uint8_t State, bool bBroadcast)
{
    Natives::UFortAssetManager_SetGameFeatureState(this, Feature, State, bBroadcast);
}

void UFortAssetManager::StartInitialLoading()
{
    StartInitialLoadingOG(this);

#if WITH_SERVER_CODE
    const uint8_t DedicatedServer = EFortGameFeature::GetDedicatedServer();
    const uint8_t Loaded = EFortGameFeatureState::GetLoaded();
    SetGameFeatureState(DedicatedServer, Loaded, true);
    Log::Info(L"UFortAssetManager::StartInitialLoading: the DedicatedServer feature ({}) is {} ({})", DedicatedServer, EFortGameFeatureState{ Loaded }.ToString(), Loaded);
#endif
}

void UFortAssetManager::StartInitialLoadingHK(UFortAssetManager* This)
{
    This->StartInitialLoading();
}

void UFortAssetManager::Hook()
{
    // 8.30 on (the game features came with it); the class's own StartInitialLoading slot, before UEngine::Init runs it.
    // 1.1-1.9 boot Core before the engine has made any object, and StaticFindObject faults there (#615); those builds
    // have no game features.
    if (!Natives::GUObjectArray || Natives::GUObjectArray->GetObjectArrayNum() == 0)
    {
        return;
    }
    if (!FindObject<UEnum>(nullptr, L"/Script/FortniteGame.EFortGameFeature") || !Finder::ResolveFortAssetManagerGameFeatures())
    {
        return;
    }
    UObject* Default = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    const int32_t Index = Natives::UFortAssetManager_StartInitialLoadingIndex;
    if (!VTable || Index < 0)
    {
        return;
    }
    StartInitialLoadingOG = reinterpret_cast<decltype(StartInitialLoadingOG)>(VTable[Index]);
    Hooks::SwapVTable(L"UFortAssetManager::StartInitialLoading", VTable, Index, reinterpret_cast<void*>(&StartInitialLoadingHK));
}
