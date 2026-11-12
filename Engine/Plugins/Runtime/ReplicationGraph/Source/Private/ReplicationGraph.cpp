#include "pch.h"
#include "ReplicationGraph.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "Natives.h"

int32_t UReplicationGraph::RemoveActorInfoFromConnections(AActor* Actor)
{
#if WITH_SERVER_CODE
    if (!Actor || !HasConnections() || !Finder::ResolveReplicationGraphActorInfoLive(*reinterpret_cast<void***>(this)))
    {
        return -1;
    }
    int32_t Removed = 0;
    for (UObject* ConnectionManager : static_cast<const TArray<UObject*>&>(Connections))
    {
        if (ConnectionManager)
        {
            // ConnectionManager->ActorInfoMap.RemoveActor(Actor): TMap::Remove, the number of entries it removed.
            void* ActorMap = reinterpret_cast<uint8_t*>(ConnectionManager) + Natives::UNetReplicationGraphConnection_ActorMapOffset;
            Removed += Natives::FPerConnectionActorInfoMap_RemoveByKey(ActorMap, Actor);
        }
    }
    return Removed;
#else
    return -1;
#endif
}
