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

struct FFortItemEntryStateValue
{
    DefineUnrealStruct(FFortItemEntryStateValue, FortniteGame)

public:
    DefineUProperty(int32_t, IntValue)
    DefineUProperty(FName, NameValue)
    DefineUProperty(uint8_t, StateType)
};

struct FFortItemEntry : public FFastArraySerializerItem
{
    DefineUnrealStruct(FFortItemEntry, FortniteGame)

public:
    static void Construct(FFortItemEntry* This, const UFortItemDefinition* InItemDefinition, int32_t InCount, int32_t InLevel);
    static uint8_t GetStateType(const wchar_t* StateName);
    static ptrdiff_t DurabilityOffset();

    int32_t GetIntValue(uint8_t StateType) const;
    void SetStateValue(uint8_t StateType, int32_t IntValue);
    bool RemoveStateValue(uint8_t StateType);
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
