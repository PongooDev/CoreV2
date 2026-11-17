#pragma once

#include "FortGameModeZone.h"

// Save the World's game modes with a primary mission (FortGameModePvE). 7.10's Frostnite mode derives from it.
class AFortGameModePvE : public AFortGameModeZone
{
    DefineUnrealClass(AFortGameModePvE, FortniteGame)
};
