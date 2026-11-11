#pragma once

#include "Athena/FortPlayerPawnAthena.h"

// A pawn a player flies by remote control, while their own pawn stays where it is (from 3.5; the Guided Missile's
// B_PrjPawn_Athena_RCRocket_C, which GA_Athena_RCRocket spawns). Its SetupRemoteControlPawn (Blueprint authority only,
// 5.41 0x12571C0) keeps the player's pawn as ControllingPlayerPawn and possesses the missile with the player's
// controller; letting go of it (0x1234EA0: the fuse, a detach or destroy, the player's pawn running out of health)
// possesses the player's pawn again with that pawn's bBeingRepossessed set around the Possess.
class AFortRemoteControlledPawnAthena : public AFortPlayerPawnAthena
{
    DefineUnrealClass(AFortRemoteControlledPawnAthena, FortniteGame)
};
