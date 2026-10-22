#pragma once

#include "UObject/Object.h"

class AFortPlayerController;
class UFortItem;
class UFortWorldItem;

class UFortItemDefinition : public UObject
{
    DefineUnrealClass(UFortItemDefinition, FortniteGame)

public:
    DefineUProperty(uint8_t, ItemType)
    DefineUProperty(int32_t, MaxStackSize)
    DefineUFunction(CreateTemporaryItemInstanceBP)

    UFortItem* CreateTemporaryItemInstanceBP(int32_t Count, int32_t Level) const;
};

class UFortWorldItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UFortWorldItemDefinition, FortniteGame)

public:
    DefineBitfieldUProperty(bCanBeDropped)

    bool ServerExecute(const UFortItem* Item, AFortPlayerController* Controller);
};

class UFortAmmoItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortAmmoItemDefinition, FortniteGame)

public:
    DefineUProperty(float, RegenCooldown)

    static ptrdiff_t RegenCooldownOffset();
};
