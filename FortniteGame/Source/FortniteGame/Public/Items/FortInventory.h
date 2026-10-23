#pragma once

#include "Containers/Array.h"
#include "StructArray.h"
#include "GameFramework/Actor.h"
#include "Items/FortItem.h"

#include <functional>
#include <vector>

class AFortPlayerController;
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
    UFortWorldItem* FindWorldItem(const FGuid& ItemGuid);
    void AddWorldItem(UFortWorldItem* WorldItem);
    void UpdateWorldItemCount(UFortWorldItem* WorldItem, int32_t NewCount);
    void UpdateWorldItemLoadedAmmo(UFortWorldItem* WorldItem, int32_t NewLoadedAmmo);
    void RemoveWorldItem(UFortWorldItem* WorldItem);
    void RemoveWorldItemCount(UFortWorldItem* WorldItem, int32_t Count);
    int32_t AddWorldItemCopy(AFortPlayerController* Owner, const FFortItemEntry& Source, int32_t Count, int32_t MaxNewItems, std::vector<UFortWorldItem*>* OutNewItems = nullptr);

    int32_t GetMaxStackSize(const UFortItemDefinition* ItemDefinition);
    int32_t GetOverflowAmount(const FFortItemEntry& ItemEntry, bool bForce);
    bool IsBackpackFull();
    int32_t AddItemStacks(const FFortItemEntry& ItemEntry, int32_t Count, int32_t MaxNewItems, bool bBackpack, bool bKeepOverflow,
        const std::function<void(UFortWorldItem* Item, int32_t Added)>& TopUp, const std::function<UFortWorldItem*(int32_t StackCount, bool bOverflow)>& NewStack);

    static constexpr int32_t MaxWorldItemStacks = 512;
};
