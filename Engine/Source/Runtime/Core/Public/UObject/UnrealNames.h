#pragma once

enum EName
{
    NAME_None = 0,
    // The channels' names from 6.10 (UE 4.22's named channels): Actor is 66h in the game's own CreateChannelByName calls.
    NAME_Actor = 102,
    NAME_Control = 255,
    NAME_Voice = 256,
    NAME_GameNetDriver = 282,
    NAME_BeaconNetDriver = 284,
    NAME_DemoNetDriver = 286,
};
