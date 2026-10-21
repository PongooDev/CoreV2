#pragma once

#include "Containers/EnumAsByte.h"
#include "GameFramework/GameState.h"
#include "UObject/AssetPtr.h"

class AFortMissionManager;
class AFortWorldManager;
class UFortMissionGenerator;
struct FFortMissionManagerRecord;

namespace EFortGameplayState
{
    enum Type
    {
        NormalGameplay,
        WaitingToStart,
        EndOfZone,
        EnteringZone,
        LeavingZone,
        Invalid,
    };
}

class AFortGameState : public AGameState
{
    DefineUnrealClass(AFortGameState, FortniteGame)

public:
    DefineUProperty(AFortMissionManager*, MissionManager)
    DefineUProperty(AFortWorldManager*, WorldManager)
    DefineUProperty(TEnumAsByte<EFortGameplayState::Type>, GameplayState)

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

    UFortMissionGenerator* GetMissionGenerator() const;
    void SetGameplayState(EFortGameplayState::Type NewGameplayState);
};
