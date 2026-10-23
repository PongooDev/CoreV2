#pragma once

#include "Engine/NetSerialization.h"
#include "Containers/Array.h"
#include "Misc/Guid.h"
#include "StructArray.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"

class AFortInventory;
class AFortPlayerController;
class UFortItemDefinition;

class EFortItemEntryState
{
    DefineUnrealEnum(EFortItemEntryState, FortniteGame)
    DefineUEnumValue(NoneState)
    DefineUEnumValue(NewItemCount)
    DefineUEnumValue(ShouldShowItemToast)
    DefineUEnumValue(DurabilityInitialized)
    DefineUEnumValue(DoNotShowSpawnParticles)
    DefineUEnumValue(FromRecoveredBackpack)
    DefineUEnumValue(FromGift)
    DefineUEnumValue(PendingUpgradeCriteriaProgress)
    DefineUEnumValue(OwnerBuildingHandle)
    DefineUEnumValue(FromDroppedPickup)
    DefineUEnumValue(CraftAndSlotTarget)
};

struct FFortItemEntryStateValue
{
    DefineUnrealStruct(FFortItemEntryStateValue, FortniteGame)

public:
    DefineUProperty(int32_t, IntValue)
    DefineUProperty(FName, NameValue)
    DefineUProperty(EFortItemEntryState, StateType)
};

struct FFortItemEntry : public FFastArraySerializerItem
{
    DefineUnrealStruct(FFortItemEntry, FortniteGame)

public:
    static void Construct(FFortItemEntry* This, const UFortItemDefinition* InItemDefinition, int32_t InCount, int32_t InLevel);
    static ptrdiff_t DurabilityOffset();

    int32_t GetIntValue(EFortItemEntryState StateType) const;
    void SetStateValue(EFortItemEntryState StateType, int32_t IntValue);
    bool RemoveStateValue(EFortItemEntryState StateType);
    bool ShouldShowItemToast() const;
    void SetNewItemCount(int32_t NewItemCount, bool bShouldShowItemToast);
    void AddNewItemCount(int32_t Count, bool bShouldShowItemToast);

    DefineUProperty(int32_t, Count)
    DefineUProperty(UFortItemDefinition*, ItemDefinition)
    DefineUProperty(float, Durability)
    DefineUProperty(int32_t, Level)
    DefineUProperty(int32_t, LoadedAmmo)
    DefineUProperty(TArray<UObject*>, AlterationDefinitions)
    DefineUProperty(FGuid, ItemGuid)
    DefineBitfieldUProperty(inventory_overflow_date)
    DefineUProperty(FWeakObjectPtr, ParentInventory)
    DefineUStructArrayProperty(FFortItemEntryStateValue, StateValues)
};

class UFortItem : public UObject
{
    DefineUnrealClass(UFortItem, FortniteGame)
    DefineUFunction(GetItemDefinitionBP)
    DefineUFunction(GetLevel)
    DefineUFunction(HasDurability)

public:
    UFortItemDefinition* GetItemDefinitionBP() const;
    int32_t GetLevel() const;
    bool HasDurability() const;
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
    bool SetItemCount(int32_t NewCount);
    bool SetLoadedAmmo(int32_t NewLoadedAmmo);
    bool SetDurability(float NewDurability);

    static ptrdiff_t ItemEntryOffset();
    static void HookVTables();

private:
    static bool SetLoadedAmmoHK(UFortWorldItem* This, int32_t NewLoadedAmmo);
    static bool SetDurabilityHK(UFortWorldItem* This, float NewDurability);
};
