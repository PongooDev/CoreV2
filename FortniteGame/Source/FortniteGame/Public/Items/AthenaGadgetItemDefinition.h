#pragma once

#include "Items/FortItemDefinition.h"

// Battle Royale's gadgets (the Glider Redeploy). 7.00 to 7.40 carry bIgnoreSingletonInventoryCheck (7.20 CL-4727874
// 0xA49); 8.00 doesn't. No exe code reads it: the check it switches is server code the build strips.
class UAthenaGadgetItemDefinition : public UFortGadgetItemDefinition
{
    DefineUnrealClass(UAthenaGadgetItemDefinition, FortniteGame)

public:
    DefineUProperty(bool, bIgnoreSingletonInventoryCheck)
    static bool HasIgnoreSingletonInventoryCheck() { return Member_bIgnoreSingletonInventoryCheck.IsAvailable(); }
};
