#pragma once

#include "UObject/Object.h"

class APlayerController;

class UPlayer : public UObject
{
    DefineUnrealClass(UPlayer, Engine)

public:
    DefineUProperty(APlayerController*, PlayerController)
    // The connection's rate in bytes a second: on the server, the client's NMT_Netspeed request clamped to the net
    // driver's MaxClientRate (UNetConnection's bandwidth limit, QueuedBits).
    DefineUProperty(int32_t, CurrentNetSpeed)
    static bool HasCurrentNetSpeed() { return Member_CurrentNetSpeed.IsAvailable(); }
    // What a client asks for over the internet (the game's ini).
    DefineUProperty(int32_t, ConfiguredInternetSpeed)
};
