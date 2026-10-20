#pragma once

#include "Containers/Array.h"
#include "StructArray.h"
#include "GameFramework/Actor.h"
#include "Items/FortItem.h"

class UFortWorldItem;

struct FFortItemList : public FFastArraySerializer
{
    DefineUnrealStruct(FFortItemList, FortniteGame)
    DefineUStructArrayProperty(FFortItemEntry, ReplicatedEntries)
    DefineUProperty(TArray<UFortWorldItem*>, ItemInstances)

public:
    FFortItemEntry* FindReplicatedEntry(const FGuid& ItemGuid);
};

class AFortInventory : public AActor
{
    DefineUnrealClass(AFortInventory, FortniteGame)

public:
    DefineUStructProperty(FFortItemList, Inventory)
    DefineUFunction(HandleInventoryLocalUpdate)

    void HandleInventoryLocalUpdate();
    void AddWorldItem(UFortWorldItem* WorldItem);
    void UpdateWorldItemCount(UFortWorldItem* WorldItem, int32_t NewCount);
    void RemoveWorldItem(UFortWorldItem* WorldItem);
};
