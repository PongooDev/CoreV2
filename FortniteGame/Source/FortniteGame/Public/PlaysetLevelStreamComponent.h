#pragma once

#include "Components/ActorComponent.h"
#include "Reflection.h"
#include "UObject/ScriptDelegates.h"

class AActor;

// What streams a playset's level in and out for an actor (from 6.20; on 7.00 every Creative island space has one): the
// server spawns the playset's level (LoadPlayset, UFortPlaysetItemDefinition::SpawnPlayset) and replicates it in
// ClientPlaysetData, which makes each client load the same level (OnRep_ClientPlaysetData).
class UPlaysetLevelStreamComponent : public UActorComponent
{
    DefineUnrealClass(UPlaysetLevelStreamComponent, FortniteGame)
    DefineUFunction(SetPlayset)

public:
    // The playset the component holds (UFortPlaysetItemDefinition), and the level it streamed in for it.
    DefineUProperty(UObject*, CurrentPlayset)
    DefineUProperty(UObject*, StreamedLevel)

    // The component's playset (the volume's CurrentPlayset with it); it streams in on the next LoadPlayset.
    void SetPlayset(UObject* NewPlayset);
    // The island queue's steps (21.40's names), the game's own (7.00 0x1325850 and 0x1338210): LoadPlayset and
    // UnloadPlayset, each returning the delegate the component broadcasts once the level is in (or out); the unload is
    // done at once (null) when no level is streamed.
    FMulticastScriptDelegate* LoadPlaysetAsyncNoCallback();
    FMulticastScriptDelegate* UnloadPlaysetAsyncNoCallback();

    // The actor's own component (one per island space), or null.
    static UPlaysetLevelStreamComponent* GetForActor(AActor* Actor);
};
