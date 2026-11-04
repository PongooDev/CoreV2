#include "pch.h"
#include "Building/BuildingWall.h"
#include "ReflectionParams.h"

bool ABuildingWall::IsDoor() const
{
    return HasDoor() && DoorMesh != nullptr;
}

void ABuildingWall::OpenDoor()
{
#if WITH_SERVER_CODE
    if (!IsDoor() || (bDoorOpen && bDoorCollisionDisabled))
    {
        return;
    }
    bDoorOpen = true;
    bDoorCollisionDisabled = true;
    for (UFunction* Function : { StaticFunction_OnRep_bDoorOpen(), StaticFunction_OnRep_bDoorCollisionDisabled() })
    {
        Reflection::FParams Params(Function, true);
        Params.Call(this);
    }
    ForceNetUpdate();
#endif
}
