#pragma once

#include "GameFramework/Actor.h"

class APlayerCameraManager : public AActor
{
    DefineUnrealClass(APlayerCameraManager, Engine)

public:
    // LimitViewRoll's bounds in ProcessViewRotation (engine default -89.9 / 89.9, 9.00 ctor 0x2F15C70).
    DefineOptionalUProperty(float, ViewRollMin)
    DefineOptionalUProperty(float, ViewRollMax)
    static bool HasViewRollLimits() { return Member_ViewRollMin.IsAvailable() && Member_ViewRollMax.IsAvailable(); }
};
