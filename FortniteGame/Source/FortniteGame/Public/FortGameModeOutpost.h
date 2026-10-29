#pragma once

#include "FortGameModeZone.h"

class AFortGameModeOutpost : public AFortGameModeZone
{
    DefineUnrealClass(AFortGameModeOutpost, FortniteGame)

public:
    // Keeps the Storm Shield's structure limit at the one the backend has for it (the admin panel's, Core-Backend #115)
    // while the zone runs.
    void KeepStructureLimit();
};
