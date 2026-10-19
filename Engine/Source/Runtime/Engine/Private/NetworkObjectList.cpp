#include "pch.h"
#include "Engine/NetworkObjectList.h"
#include "Natives.h"

void FNetworkObjectList::Remove(AActor* const Actor)
{
    Natives::FNetworkObjectList_Remove(this, Actor);
}
