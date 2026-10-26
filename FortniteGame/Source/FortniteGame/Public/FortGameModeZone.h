#pragma once

#include "FortGameMode.h"

class AFortAIDirector;
class APlayerController;
class UPlayer;
struct FUniqueNetIdRepl;
class AFortAIGoalManager;
class AFortVisibilityManager;
class UEnvQueryInstanceBlueprintWrapper;
struct FFortPlayerSpawnPadPlacementData;
class UFortItemDefinition;

struct FItemAndCount
{
    DefineUnrealStruct(FItemAndCount, FortniteGame)
    DefineUProperty(int32_t, Count)
    DefineUProperty(UFortItemDefinition*, Item)
};

class AFortGameModeZone : public AFortGameMode
{
    DefineUnrealClass(AFortGameModeZone, FortniteGame)

public:
    DefineUProperty(AFortAIDirector*, AIDirector)
    DefineUProperty(AFortAIGoalManager*, AIGoalManager)
    DefineUProperty(AActor*, ActiveSpawnPad)
    DefineUProperty(int32_t, EndOfZoneRemainTime)
    DefineUProperty(int32_t, OverrideAIDirectorIndex)
    DefineUProperty(TSubclassOf<AFortVisibilityManager>, VisibilityManagerClass)
    // What a player starts a match with (Battle Royale's: the pickaxe, the building pieces, the edit tool and the
    // materials, Athena_GameMode). No native code reads it in the shipping exe.
    DefineUStructArrayProperty(FItemAndCount, StartingItems)

    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    UClass* DetermineAIDirectorClass() const;
    void CreateAIDirector();
    void PostInitializeComponents();
    APlayerController* Login(UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage);
    void FinishWorldInitialization(AFortWorldManager* WorldManager);
    FFortPlayerSpawnPadPlacementData* GetPlayerSpawnPadPlacementData();
    void PlacePlayerSpawnPad(AFortWorldManager* WorldManager);
    void OnPlayerSpawnPadQueryFinished(AFortWorldManager* WorldManager, UEnvQueryInstanceBlueprintWrapper* PlacementQuery);

    void OnZoneEnd();

    static ptrdiff_t EndOfZoneRemainTimeOffset();
    static ptrdiff_t ActiveSpawnPadOffset();

    static void Hook();
    static void HookVTables();

private:
    static APawn* SpawnDefaultPawnFor_ImplementationHK(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot);
    static AActor* PickFriendlyPlayerSpawnActorCall(AFortGameModeZone* This, AController* Player, UClass* PawnClass, TArray<AActor*>* Candidates, float DangerExclusionRadius, float ObjectiveExclusionRadius);
    static AActor* PickInitialPlayerSpawnActorCall(AFortGameModeZone* This, AController* Player, UClass* PawnClass, TArray<AActor*>* Candidates, float DangerExclusionRadius, float ObjectiveExclusionRadius);
    static AActor* PickBackupPlayerSpawnActorCall(AFortGameModeZone* This, AController* Player, UClass* PawnClass, TArray<AActor*>* Candidates, float DangerExclusionRadius, float ObjectiveExclusionRadius);

    void GatherPlayerSpawnActors(AController* Player, bool bInitialTags, bool bNearFriendlyPlayers, TArray<AActor*>& OutCandidates);
    static void FinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager);
    static void SubclassFinishWorldInitializationHK(AFortGameModeZone* This, AFortWorldManager* WorldManager);
    static void CreateAIDirectorHK(AFortGameModeZone* This);
    static inline void (*PostInitializeComponentsOG)(AFortGameModeZone* This) = nullptr;
    static void PostInitializeComponentsHK(AFortGameModeZone* This);
    static inline APlayerController* (*LoginOG)(AFortGameModeZone* This, UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) = nullptr;
    static APlayerController* LoginHK(AFortGameModeZone* This, UPlayer* NewPlayer, ENetRole InRemoteRole, const FString& Portal, const FString& Options, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage);
};
