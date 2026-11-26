#include "pch.h"
#include "FortReplicationGraph.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "Natives.h"
#include "Templates/Casts.h"

namespace
{
    // PastRelevantActors (9.00 +0x278): TArray<FFortAlwaysRelevantActorInfo, TInlineAllocator<4>>, the four inline
    // entries first, then the heap data once it grows past them, then ArrayNum (GatherActorListsForConnection 0x16BB624:
    // lea rbx, [rdi+278h]; mov rax, [rbx+40h]; movsxd rdx, dword ptr [rbx+48h]; cmovz r15, rbx).
    struct FPastRelevantActorsView
    {
        FFortAlwaysRelevantActorInfo InlineData[4];
        FFortAlwaysRelevantActorInfo* SecondaryData;
        int32_t ArrayNum;
        int32_t ArrayMax;

        FFortAlwaysRelevantActorInfo* GetData() { return SecondaryData ? SecondaryData : InlineData; }
    };
    static_assert(sizeof(FPastRelevantActorsView) == 0x50);
}
// 9.01 (CL-6428087, GatherActorListsForConnection 0x16BECE0) and 21.40 have PastRelevantActors as a plain
// TArray<FFortAlwaysRelevantActorInfo>.

int32_t UFortReplicationGraphNode_AlwaysRelevant_ForConnection::ForgetDestroyedPawn(UReplicationGraph* Graph, AActor* Pawn)
{
#if WITH_SERVER_CODE
    if (!Graph || !Pawn || !UReplicationGraph::HasConnections() || !UNetReplicationGraphConnection::HasConnectionGraphNodes() || !StaticClass()
        || !Finder::ResolveAlwaysRelevantPastActorsLive())
    {
        return -1;
    }
    int32_t Forgotten = 0;
    for (UObject* ConnectionObject : static_cast<const TArray<UObject*>&>(Graph->Connections))
    {
        UNetReplicationGraphConnection* ConnectionManager = Cast<UNetReplicationGraphConnection>(ConnectionObject);
        if (!ConnectionManager)
        {
            continue;
        }
        for (UReplicationGraphNode* Node : static_cast<const TArray<UReplicationGraphNode*>&>(ConnectionManager->ConnectionGraphNodes))
        {
            if (!Cast<UFortReplicationGraphNode_AlwaysRelevant_ForConnection>(Node))
            {
                continue;
            }
            uint8_t* const PastRelevantActorsAddress = reinterpret_cast<uint8_t*>(Node) + Natives::UFortReplicationGraphNode_AlwaysRelevant_ForConnection_PastRelevantActorsOffset;
            FFortAlwaysRelevantActorInfo* Infos = nullptr;
            int32_t Num = 0;
            if (Natives::UFortReplicationGraphNode_AlwaysRelevant_ForConnection_bPastRelevantActorsInline)
            {
                FPastRelevantActorsView& PastRelevantActors = *reinterpret_cast<FPastRelevantActorsView*>(PastRelevantActorsAddress);
                Infos = PastRelevantActors.GetData();
                Num = PastRelevantActors.ArrayNum;
            }
            else
            {
                TArray<FFortAlwaysRelevantActorInfo>& PastRelevantActors = *reinterpret_cast<TArray<FFortAlwaysRelevantActorInfo>*>(PastRelevantActorsAddress);
                Infos = PastRelevantActors.GetData();
                Num = PastRelevantActors.Num();
            }
            for (int32_t Index = 0; Infos && Index < Num; ++Index)
            {
                if (Infos[Index].LastPawn == Pawn)
                {
                    Infos[Index].LastPawn = nullptr;
                    ++Forgotten;
                }
            }
        }
    }
    return Forgotten;
#else
    return -1;
#endif
}
