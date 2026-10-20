#pragma once

#include "FortGameMode.h"

class AFortAIDirector;
class APlayerController;
class UPlayer;
struct FUniqueNetIdRepl;
class AFortAIGoalManager;
class UEnvQueryInstanceBlueprintWrapper;

class AFortGameModeZone : public AFortGameMode
{
    DefineUnrealClass(AFortGameModeZone, FortniteGame)

public:
    DefineUProperty(AFortAIDirector*, AIDirector)
    DefineUProperty(AFortAIGoalManager*, AIGoalManager)
    DefineUProperty(AActor*, ActiveSpawnPad)
    DefineUProperty(int32_t, EndOfZoneRemainTime)
    DefineUProperty(int32_t, OverrideAIDirectorIndex)

    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    UClass* DetermineAIDirectorClass() const;
    void CreateAIDirector();
    APlayerController* Login(UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage);
    void FinishWorldInitialization(AFortWorldManager* WorldManager);
    void PlacePlayerSpawnPad(AFortWorldManager* WorldManager);
    void OnPlayerSpawnPadQueryFinished(AFortWorldManager* WorldManager, UEnvQueryInstanceBlueprintWrapper* PlacementQuery);

    void OnZoneEnd();

    static ptrdiff_t EndOfZoneRemainTimeOffset();
    static ptrdiff_t ActiveSpawnPadOffset();

    static void Hook();
    static void HookVTables();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot);
    static void FinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager);
    static void CreateAIDirectorHK(AFortGameModeZone* This);
    static inline APlayerController* (*LoginOG)(AFortGameModeZone* This, UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) = nullptr;
    static APlayerController* LoginHK(AFortGameModeZone* This, UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage);
};
