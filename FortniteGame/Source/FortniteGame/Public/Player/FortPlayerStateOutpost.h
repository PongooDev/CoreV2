#pragma once

#include "Player/FortPlayerState.h"

class AFortPlayerStateOutpost : public AFortPlayerState
{
    DefineUnrealClass(AFortPlayerStateOutpost, FortniteGame)
    DefineBitfieldUProperty(bCanEditOutpost)
    DefineNativeBitfield(bIsOutpostOwner, AFortPlayerStateOutpost_bIsOutpostOwner, Natives::AFortPlayerStateOutpost_bIsOutpostOwnerMask)

public:
    bool IsOutpostOwner() const { return bIsOutpostOwner; }
};
