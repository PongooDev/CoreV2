#pragma once

#include "STW/FortGameModePvE.h"

class AFortPlayerController;

// Frostnite's game mode (7.10's Endless_GameMode, Zone_Endless_Winter_Polar). The class has no fields, and the client
// build has none of its code: its class constructor (7.10 CL-4667333 0x15951B0) sets AFortGameModePvE's vtable
// (0x3CDD0C8) on it, so every one of its virtuals was server-only and the linker folded what was left into its parent's.
class AFortGameModeEndless : public AFortGameModePvE
{
    DefineUnrealClass(AFortGameModeEndless, FortniteGame)

public:
    // A player has begun play in the zone (the game's NewPlayerHasBegunPlay mission event, which Mission_Endless itself
    // answers with the player's starting inventory): starts Frostnite's primary mission once.
    void HandleNewPlayerHasBegunPlay(AFortPlayerController* PlayerController);

private:
    // The StartPrimaryMission mission event that Mission_Endless's HandleMissionEvent_StartPrimaryMission waits for.
    void StartPrimaryMission();
};
