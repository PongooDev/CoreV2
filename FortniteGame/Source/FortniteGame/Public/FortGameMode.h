#pragma once

#include "GameFramework/GameMode.h"
#include "GameplayTagContainer.h"
#include "Items/FortQuickBars.h"
#include "UObject/AssetPtr.h"

class AActor;
class AController;
class AFortGameSession;
class AFortPlayerController;
class AFortWorldManager;
class UFortSharedMissionLists;
class UFortPlaylistManager;
class APawn;
class FScriptArray;
struct FAccountIdAndScore;
template <typename T>
class TArray;

// What dying or being downed costs a player (Save the World: DBNODeathPenalty), the game mode's DeathPenaltyData.
class UFortGameDeathPenalty : public UObject
{
    DefineUnrealClass(UFortGameDeathPenalty, FortniteGame)

public:
    DefineUProperty(float, SelfResurrectDurabilityPenalty)
    DefineUProperty(float, TeammateResurrectDurabilityPenalty)
};

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
    // The playlist's name with its id (SetCurrentPlaylistName sets both), the matchmaking service's bucket of the match
    // the server was assigned ("Fortnite:Fortnite:<netcl>:0:<region>:<playlist>...", which the game session keeps here
    // from the assignment) and the game mode's playlists.
    DefineUProperty(FName, CurrentPlaylistName)
    DefineUProperty(FString, CurrentBucketId)
    DefineUProperty(UFortPlaylistManager*, PlaylistManager)
    // From 4.0 the game mode keeps the asset's path in DefaultDeathPenaltyData and the loaded asset in DeathPenaltyData.
    DefineUProperty(TAssetPtr<UFortGameDeathPenalty>, DefaultDeathPenaltyData)
    DefineUProperty(UFortGameDeathPenalty*, DeathPenaltyData)

private:
    // Before 4.0, DeathPenaltyData was the asset's path (a TAssetPtr), with no DefaultDeathPenaltyData.
    static inline Reflection::FMemberInfo Member_DeathPenaltyDataPath{ &StaticReflectionOwner, TEXT("DeathPenaltyData"), sizeof(TAssetPtr<UFortGameDeathPenalty>), Reflection::EMemberKind::Value, nullptr, Reflection::SoftPathGrowth<TAssetPtr<UFortGameDeathPenalty>> };

public:
    // The death penalty (Save the World's resurrect durability costs), loading it from its path if need be; null when
    // the game mode has none.
    UFortGameDeathPenalty* GetDeathPenaltyData();

    TSubclassOf<AGameSession> GetGameSessionClass() const;
    APawn* SpawnDefaultPawnFor_Implementation(AController* NewPlayer, AActor* StartSpot);
    void FinishWorldInitialization(AFortWorldManager* WorldManager);
    void SetBadgeScores();
    // Scores the players and grants it. Before 5.21 the game sends it as EarnScore; 5.21 (EndMatch) has no such send,
    // so the scores go in OutPlayersAndScores for EndMatch to carry.
    void SendEarnScore(TArray<FAccountIdAndScore>* OutPlayersAndScores = nullptr);
    UFortSharedMissionLists* GetSharedMissionLists();
    // The location tags of where Actor is (Battle Royale's POI volumes: Athena.Location.POI.FlushFactory...), added to
    // OutTags, as the stat senders add them to their events. Nothing on builds before 3.0 or outside Battle Royale.
    void GetPoiTagsFor(AActor* Actor, FGameplayTagContainer& OutTags);
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
