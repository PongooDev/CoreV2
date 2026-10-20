#pragma once

#include "Engine/NetSerialization.h"
#include "Misc/Guid.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"

class AFortInventory;
class AFortPlayerController;
class UFortItemDefinition;

struct FFortItemEntry : public FFastArraySerializerItem
{
    DefineUnrealStruct(FFortItemEntry, FortniteGame)

public:
    static void Construct(FFortItemEntry* This, const UFortItemDefinition* InItemDefinition, int32_t InCount, int32_t InLevel);

    DefineUProperty(int32_t, Count)
    DefineUProperty(UFortItemDefinition*, ItemDefinition)
    DefineUProperty(int32_t, LoadedAmmo)
    DefineUProperty(FGuid, ItemGuid)
    DefineUProperty(FWeakObjectPtr, ParentInventory)
};

class UFortItem : public UObject
{
    DefineUnrealClass(UFortItem, FortniteGame)
    DefineUFunction(GetItemDefinitionBP)
    DefineUFunction(GetLevel)

public:
    UFortItemDefinition* GetItemDefinitionBP() const;
    int32_t GetLevel() const;
};

class UFortWorldItem : public UFortItem
{
    DefineUnrealClass(UFortWorldItem, FortniteGame)

public:
    DefineUProperty(AFortInventory*, OwnerInventory)
    DefineUStructProperty(FFortItemEntry, ItemEntry)
    DefineBitfieldUProperty(bNeedsPersistentUpdate)
    DefineBitfieldUProperty(bIsTemporaryItem)
    DefineUFunction(SetOwningControllerForTemporaryItem)

    void SetOwningControllerForTemporaryItem(AFortPlayerController* InController);
    bool SetLoadedAmmo(int32_t NewLoadedAmmo);

    static void HookVTables();

private:
    static bool SetLoadedAmmoHK(UFortWorldItem* This, int32_t NewLoadedAmmo);
};
