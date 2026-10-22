#pragma once

#include "GameFramework/GameMode.h"

class AActor;
class AController;
class AFortGameSession;
class AFortPlayerController;
class AFortWorldManager;
class APawn;
class FScriptArray;
struct FAccountIdAndScore;
template <typename T>
class TArray;

class AFortGameMode : public AGameMode
{
    DefineUnrealClass(AFortGameMode, FortniteGame)

public:
    DefineUProperty(AFortGameSession*, FortGameSession)
    DefineBitfieldUProperty(bWorldIsReady)
    DefineUProperty(UClass*, MissionManagerClass)

    TSubclassOf<AGameSession> GetGameSessionClass() const;
    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    void FinishWorldInitialization(AFortWorldManager* WorldManager);
    void SetBadgeScores();
    void SendEarnScore();

    static bool ShouldPlayerEarnScore(AFortPlayerController* PlayerController);
    static int32_t GetPlayerBadgesScore(AFortPlayerController* PlayerController);
    static void SendEarnScoreCommand(AFortPlayerController* SendingPlayer, const TArray<FAccountIdAndScore>& PlayersAndScores);

    static void Hook();
    static void HookCallSites();

private:
    static TSubclassOf<AGameSession>* GetGameSessionClassHK(const AFortGameMode* This, TSubclassOf<AGameSession>* Result);
    static void ResizeGrowPlayersAndScoresCall(FScriptArray* PlayersAndScores, int32_t OldNum);
    static void* MakeEarnScoreContextCall(void* Result, void* Players);

    static inline FScriptArray* PendingPlayersAndScores = nullptr;
};
