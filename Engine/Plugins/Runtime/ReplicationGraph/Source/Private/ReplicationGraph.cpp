#include "pch.h"
#include "ReplicationGraph.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "Log.h"
#include "Natives.h"
#include "Templates/Casts.h"

#include <format>
#include <map>
#include <string>

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

void UReplicationGraph::HandleActorSpawned(AActor* Actor)
{
#if WITH_SERVER_CODE
    UWorld* World = Actor ? Actor->GetWorld() : nullptr;
    UNetDriver* Driver = World ? World->GetNetDriver() : nullptr;
    UReplicationGraph* Graph = Driver && Driver->HasReplicationDriver() ? Cast<UReplicationGraph>(reinterpret_cast<UObject*>(static_cast<UReplicationDriver*>(Driver->ReplicationDriver))) : nullptr;
    const int32_t Removed = Graph ? Graph->RemoveActorInfoFromConnections(Actor) : -1;
    if (Removed <= 0)
    {
        return;
    }
    // The first of each class and every hundredth after it: pickups take each other's addresses all match.
    static std::map<std::wstring, int32_t> Forgotten;
    static int32_t Total = 0;
    ++Total;
    const std::wstring ClassName = Actor->GetClass()->GetName().ToStdString();
    const int32_t OfClass = ++Forgotten[ClassName];
    if (OfClass == 1 || OfClass % 100 == 0)
    {
        Log::Info(L"ReplicationGraph: {} is spawned at 0x{:X}, where {} connection{} still had a destroyed actor's info (a dormant or torn off one's would keep the new actor from that player): forgotten ({} for {} so far, {} in all)",
            Actor->GetName().ToStdString(), reinterpret_cast<uintptr_t>(Actor), Removed, Removed == 1 ? L"" : L"s", OfClass, ClassName, Total);
    }
#endif
}
