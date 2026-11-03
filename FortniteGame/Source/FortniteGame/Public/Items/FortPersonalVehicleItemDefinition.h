#pragma once

#include "Items/FortItemDefinition.h"
#include "UObject/AssetPtr.h"

class UGameplayAbility;

// 3.0's (CL-3901517) personal vehicle, an account item (Epic's derives from UFortAccountItemDefinition): the Save the
// World Hoverboard, VID_Hoverboard (PersonalVehicleAbility GA_Hoverboard, MountTime 3). The ability does the riding;
// there is no vehicle actor.
class UFortPersonalVehicleItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UFortPersonalVehicleItemDefinition, FortniteGame)

public:
    // Private in Epic's class (no accessor survives in 3.0), read by reflection.
    DefineUProperty(TAssetSubclassOf<UGameplayAbility>, PersonalVehicleAbility)
    // The seconds the client holds the use key before it asks for the vehicle (its own timer, 3.0 0xE49090).
    DefineUProperty(float, MountTime)
};
