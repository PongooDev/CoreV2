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
    DefineUProperty(TAssetSubclassOf<UFortMissionGenerator>, MissionGeneratorClass)
    DefineUProperty(FString, TheaterUniqueId)
    DefineUProperty(UFortZoneTheme*, ZoneTheme)
    DefineUProperty(UBuildingStructuralSupportSystem*, StructuralSupportSystem)

    UFortMissionGenerator* GetMissionGenerator() const;
    void SetGameplayState(EFortGameplayState NewGameplayState);
};
