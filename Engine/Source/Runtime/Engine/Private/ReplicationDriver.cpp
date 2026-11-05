#include "pch.h"
#include "Engine/ReplicationDriver.h"
#include "Finder.h"
#include "Natives.h"
#include "ObjectLayout.h"

int32_t UReplicationDriver::ServerReplicateActors(float DeltaSeconds)
{
    // A virtual (PURE_VIRTUAL on UReplicationDriver): the replication graph's own implementation, at the slot the finder
    // reads from UReplicationDriver's default object.
    if (!Finder::ResolveReplicationDriverLive())
    {
        return 0;
    }
    return CallVirtual<int32_t>(this, Natives::UReplicationDriver_ServerReplicateActorsIndex, DeltaSeconds);
}
