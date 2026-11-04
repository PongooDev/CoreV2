#pragma once

#include "GameFramework/GameModeBase.h"

class AGameMode : public AGameModeBase
{
    DefineUnrealClass(AGameMode, Engine)
    DefineUFunction(StartMatch)

public:
    // Moves the match from WaitingToStart to InProgress (HandleMatchHasStarted), whatever ReadyToStartMatch says.
    void StartMatch();
};
