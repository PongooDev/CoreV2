#include "pch.h"
#include "Net/DataReplication.h"
#include "Natives.h"

bool FObjectReplicator::ValidateAgainstState(const UObject* ObjectState)
{
    return Natives::FObjectReplicator_ValidateAgainstState(this, ObjectState);
}
