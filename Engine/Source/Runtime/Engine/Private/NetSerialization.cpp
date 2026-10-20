#include "pch.h"
#include "Engine/NetSerialization.h"
#include "Natives.h"

void FFastArraySerializer::MarkArrayDirty()
{
    Natives::FFastArraySerializer_MarkArrayDirty(this);
}
