#pragma once

#include "UObject/Object.h"

class APlayerController;

// A player controller's cheats: the console's exec functions while the controller has one (AddCheats).
class UCheatManager : public UObject
{
    DefineUnrealClass(UCheatManager, Engine)

public:
    APlayerController* GetOuterAPlayerController() const;
};
