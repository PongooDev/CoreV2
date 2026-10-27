#pragma once

#include "GameFramework/GameMode.h"
#include "GameplayTagContainer.h"
#include "Items/FortQuickBars.h"

class AActor;
class AController;
class AFortGameSession;
class AFortPlayerController;
class AFortWorldManager;
class UFortSharedMissionLists;
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
    DefineUProperty(FGameplayTagContainer, GameContextTags)
    DefineUProperty(UFortSharedMissionLists*, SharedMissionLists)
    // From 1.7.2 (Battle Royale's quickbars: one harvest and five weapon slots, then the building pieces).
    DefineUProperty(bool, bOverrideQuickBars)
    DefineUStructProperty(FQuickBarData, QuickBarDefinitions)
    // The match's playlist (Battle Royale: 2 solo, 10 duo, 9 squad), what the game mode records its winners with.
    DefineUProperty(int32_t, CurrentPlaylistId)

    TSubclassOf<AGameSession> GetGameSessionClass() const;
    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    void FinishWorldInitialization(AFortWorldManager* WorldManager);
    void SetBadgeScores();
    void SendEarnScore();
    UFortSharedMissionLists* GetSharedMissionLists();
    // The game mode's own quickbars (1.7.2's native quickbar setup, 0x6813D0, reads them before GameData's).
    bool OverridesQuickBars();
    const FQuickBarData& GetQuickBarDefinition(EFortQuickBars InQuickBar) const
    {
        const FQuickBarData& Definitions = QuickBarDefinitions;
        const uint8_t* Definition = reinterpret_cast<const uint8_t*>(&Definitions) + static_cast<size_t>(InQuickBar) * FQuickBarData::StaticStruct()->GetStructureSize();
        return *reinterpret_cast<const FQuickBarData*>(Definition);
    }

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
