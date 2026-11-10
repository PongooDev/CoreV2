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
    DefineUFunction(Teleport)

public:
    APlayerController* GetOuterAPlayerController() const;
    // Runs each command of the game ini's [CheatScript.<ScriptName>] section on the outer player controller.
    void CheatScript(const FString& ScriptName);
    // UCheatManager's own body (5.30 0x2383C40, 5.21 0x2344690): a trace from the controller's view point along its view
    // ("TeleportTrace"), and the pawn's TeleportTo there. Called through UCheatManager's vtable, never this object's, so
    // a subclass's override can run it as Super::Teleport.
    void Teleport();
};
