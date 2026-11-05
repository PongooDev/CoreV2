#pragma once

#include "UObject/Object.h"

// UE 4.20's UReplicationDriver (3.3 has it, as UNetDriver::ReplicationDriver): a net driver with one hands its whole
// ServerReplicateActors to it. Fortnite's is UFortReplicationGraph, off on 3.3 and on for 3.4's Battle Royale.
class UReplicationDriver : public UObject
{
    DefineUnrealClass(UReplicationDriver, Engine)

public:
    int32_t ServerReplicateActors(float DeltaSeconds);
};
