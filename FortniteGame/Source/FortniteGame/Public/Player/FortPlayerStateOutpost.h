#pragma once

#include "Player/FortPlayerState.h"

class AFortPlayerStateOutpost : public AFortPlayerState
{
    DefineUnrealClass(AFortPlayerStateOutpost, FortniteGame)
    // 9.00 renamed bCanEditOutpost (8.51 +0xC50) to bHasPermissionToEditOutpost (+0xC50).
    DefineBitfieldUProperty(bCanEditOutpost)
    DefineBitfieldUProperty(bHasPermissionToEditOutpost)
    DefineNativeBitfield(bIsOutpostOwner, AFortPlayerStateOutpost_bIsOutpostOwner, Natives::AFortPlayerStateOutpost_bIsOutpostOwnerMask)

public:
    bool IsOutpostOwner() const { return bIsOutpostOwner; }
    void SetHasPermissionToEditOutpost(bool bHasPermission)
    {
        if (Member_bHasPermissionToEditOutpost.IsAvailable())
        {
            bHasPermissionToEditOutpost = bHasPermission;
        }
        else if (Member_bCanEditOutpost.IsAvailable())
        {
            bCanEditOutpost = bHasPermission;
        }
    }
};
