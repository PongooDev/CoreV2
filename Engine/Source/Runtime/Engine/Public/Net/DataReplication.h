#pragma once

#include "ObjectLayout.h"

class UActorChannel;
class UObject;

class FObjectReplicator
{
public:
    DefineNativeMember(UActorChannel*, OwningChannel, FObjectReplicator_OwningChannel)

    bool ValidateAgainstState(const UObject* ObjectState);
};
