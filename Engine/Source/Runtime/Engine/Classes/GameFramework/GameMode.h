#pragma once

#include "GameFramework/GameModeBase.h"

class AGameMode : public AGameModeBase
{
    DefineUnrealClass(AGameMode, Engine)
    DefineUFunction(StartMatch)

public:
    // The player controllers in the game (PostLogin and Logout count them; no AI controller), and those still travelling.
    DefineUProperty(int32_t, NumPlayers)
    DefineUProperty(int32_t, NumTravellingPlayers)
    static bool HasNumPlayers() { return StaticClass() && Member_NumPlayers.IsAvailable() && Member_NumTravellingPlayers.IsAvailable(); }

    // Moves the match from WaitingToStart to InProgress (HandleMatchHasStarted), whatever ReadyToStartMatch says.
    void StartMatch();
    // AGameMode::GetNumPlayers (6.21 0x2634980, AFortGameModeAthena's slot 0xCE): NumPlayers + NumTravellingPlayers.
    int32_t GetNumPlayers() const { return NumPlayers + NumTravellingPlayers; }
};
