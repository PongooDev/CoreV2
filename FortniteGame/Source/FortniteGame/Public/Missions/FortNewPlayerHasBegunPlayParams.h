#pragma once

#include "UObject/Object.h"

class AFortPlayerController;

// The params of the "NewPlayerHasBegunPlay" mission event (7.10 adds the class; earlier builds don't have it, and
// StaticClass() is null there). Frostnite's Mission_Endless answers the event with GiveStartingInventoryToPlayers.
class UFortNewPlayerHasBegunPlayParams : public UObject
{
    DefineUnrealClass(UFortNewPlayerHasBegunPlayParams, FortniteGame)
    DefineUFunction(SetParams)

public:
    UFortNewPlayerHasBegunPlayParams* SetParams(AFortPlayerController* NewPlayerPC);

    static void SendNewPlayerHasBegunPlay(AFortPlayerController* NewPlayerPC);
};
