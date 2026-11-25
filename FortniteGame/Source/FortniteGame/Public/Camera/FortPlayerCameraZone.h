#pragma once

#include "Camera/FortPlayerCameraBase.h"

class AFortPlayerCameraZone : public AFortPlayerCameraBase
{
    DefineUnrealClass(AFortPlayerCameraZone, FortniteGame)

public:
    static void HookVTables();
};
