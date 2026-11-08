#pragma once

#include "Containers/UnrealString.h"
#include "UObject/Object.h"

class APlayerController;

// A player controller's cheats: the console's exec functions while the controller has one (AddCheats).
class UCheatManager : public UObject
{
    DefineUnrealClass(UCheatManager, Engine)
    DefineUFunction(Fly)
    DefineUFunction(God)
    DefineUFunction(CheatScript)

public:
    APlayerController* GetOuterAPlayerController() const;
    // Runs each command of the game ini's [CheatScript.<ScriptName>] section on the outer player controller.
    void CheatScript(const FString& ScriptName);
};
