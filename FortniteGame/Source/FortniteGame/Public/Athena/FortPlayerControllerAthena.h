#pragma once

#include "Containers/Array.h"
#include "Player/FortPlayerController.h"

class UFortHero;
class UFortHeroType;

// Battle Royale's player controller (from 1.7.2; Athena_PlayerController_C).
class AFortPlayerControllerAthena : public AFortPlayerController
{
    DefineUnrealClass(AFortPlayerControllerAthena, FortniteGame)

public:
    // The heroes Battle Royale plays (Athena_PlayerController_C: HID_Commando_Athena_*).
    DefineUProperty(TArray<UFortHeroType*>, DefaultHeroes)

    bool IsDefaultHero(UFortHero* Hero);
    UFortHero* GetOrInitializeHero();
};
