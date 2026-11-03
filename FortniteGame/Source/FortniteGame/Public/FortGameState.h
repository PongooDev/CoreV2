#pragma once

#include "GameFramework/GameState.h"
#include "UObject/AssetPtr.h"

class AFortMissionManager;
class AFortVisibilityManager;
class AFortWorldManager;
class UBuildingStructuralSupportSystem;
class UFortMissionGenerator;
class UFortZoneTheme;
struct FFortMissionManagerRecord;

class EFortGameplayState
{
    DefineUnrealEnum(EFortGameplayState, FortniteGame)
    DefineUEnumValue(NormalGameplay)
    DefineUEnumValue(WaitingToStart)
    DefineUEnumValue(EndOfZone)
    DefineUEnumValue(EnteringZone)
    DefineUEnumValue(LeavingZone)
    DefineUEnumValue(Invalid)
};

class AFortGameState : public AGameState
{
    DefineUnrealClass(AFortGameState, FortniteGame)

public:
    DefineUProperty(AFortMissionManager*, MissionManager)
    DefineUProperty(AFortWorldManager*, WorldManager)
    DefineUProperty(int32_t, WorldLevel)
    DefineUProperty(AFortVisibilityManager*, VisibilityManager)
    DefineUProperty(EFortGameplayState, GameplayState)

    void SpawnMissionManager(const FFortMissionManagerRecord& MissionManagerRecord);

    static ptrdiff_t MissionManagerOffset();
};

class AFortGameStateZone : public AFortGameState
{
    DefineUnrealClass(AFortGameStateZone, FortniteGame)

public:
    DefineUProperty(float, GameDifficulty)
    // The BluGlo Difficulty Pylon's tier (2.5+; Mission_BluGloPylon's SetDifficultyIncreaseRewardTier).
    DefineUProperty(int32_t, DifficultyIncreaseRewardTier)
    DefineUProperty(TAssetSubclassOf<UFortMissionGenerator>, MissionGeneratorClass)
    DefineUProperty(FString, TheaterUniqueId)
    DefineUProperty(UFortZoneTheme*, ZoneTheme)
    DefineUProperty(UBuildingStructuralSupportSystem*, StructuralSupportSystem)
    DefineUProperty(int32_t, TotalPlayerStructures)
    DefineUProperty(int32_t, MaxPlayerStructures)

    UFortMissionGenerator* GetMissionGenerator() const;
    // The difficulty increase tier the zone's BluGlo Difficulty Pylon reached, which EndPrimaryMission reports; 0 when it
    // wasn't used or the build has no pylon.
    int32_t GetDifficultyIncreaseRewardTier() const;
    // The zone's structure count and limit (-1: none). Building is refused once the count reaches the limit
    // (AFortPlayerController::CanPlaceBuildableClassInStructuralGrid, 1.8 0x816430); traps never ask.
    bool GetPlayerStructures(int32_t& OutTotal, int32_t& OutMax) const;
    // Sets the limit (MaxPlayerStructures, replicated: the HUD's "structures left" follows), as the game's own
    // AFortGameModeOutpost::ApplyStructureLimit does (1.8 0x57C440, a plain store). False without the property.
    bool SetMaxPlayerStructures(int32_t NewMax);
    void SetGameplayState(EFortGameplayState NewGameplayState);
};
