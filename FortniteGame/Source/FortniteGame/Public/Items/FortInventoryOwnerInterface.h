#pragma once

#include "Misc/Guid.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "UObject/Object.h"

class AFortInventory;

class UFortInventoryOwnerInterface : public UObject
{
    DefineUnrealClass(UFortInventoryOwnerInterface, FortniteGame)
};

class IFortInventoryOwnerInterface
{
public:
    virtual ~IFortInventoryOwnerInterface() = default;
    virtual UObject* _getUObject() const = 0;

    AFortInventory* GetWorldInventory()
    {
        return CallVirtual<AFortInventory*>(this, Natives::IFortInventoryOwnerInterface_GetWorldInventoryIndex);
    }
};
