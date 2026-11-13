#pragma once

#include "Engine/NetSerialization.h"
#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Misc/Guid.h"
#include "StructArray.h"
#include "UObject/Object.h"
#include "UObject/WeakObjectPtr.h"

class AFortInventory;
class AFortPlayerController;
class EFortInventoryFilter;
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
    // Loads a ranged weapon's full magazine (Battle Royale's loot), and a consumable weapon's whose charges are a separate
    // ammo item (6.21's Balloons). False for anything else.
    bool LoadFullClip();

    // The entry's perks (alterations): AlterationDefinitions, their template ids ("Alteration:AID_Ele_Fire_T01"), which
    // is what the MCP saves (SaveGame, "alterationDefinitions"), and AlterationInstances, the definitions they name
    // (Transient). UFortWorldItem::PopulateFromMcpItem (4.2 0xD6D930) fills the instances from the template ids when the
    // backpack loads (0xD40A00); every other entry the server makes has to be given both. The world item's
    // GetActiveAlterations reads the instances: AFortWeapon takes its AppliedAlterations from them (0x1073C60) and a
    // placed trap its own (ABuildingTrap::SpawnedFromItem, 0xAFC240). False when the build has neither member as these
    // types (template id strings from 3.0 at the latest).
    static bool HasAlterations();
    bool SetAlterations(const TArray<FString>& TemplateIds, const TArray<UObject*>& Definitions);
    // Source's perks into this entry, as a copy of the entry carries them.
    void CopyAlterationsFrom(const FFortItemEntry& Source);

    DefineUProperty(int32_t, Count)
    DefineUProperty(UFortItemDefinition*, ItemDefinition)
    DefineUProperty(float, Durability)
    DefineUProperty(int32_t, Level)
    DefineUProperty(int32_t, LoadedAmmo)
    DefineUProperty(FGuid, ItemGuid)
    DefineBitfieldUProperty(inventory_overflow_date)
    DefineUProperty(bool, bIsDirty)
    DefineUProperty(FWeakObjectPtr, ParentInventory)
    DefineUStructArrayProperty(FFortItemEntryStateValue, StateValues)

private:
    DefineUProperty(TArray<FString>, AlterationDefinitions)
    DefineUProperty(TArray<UObject*>, AlterationInstances)
};

class UFortItem : public UObject
{
    DefineUnrealClass(UFortItem, FortniteGame)
    DefineUFunction(GetItemDefinitionBP)
    DefineUFunction(GetLevel)
    DefineUFunction(HasDurability)
    DefineUFunction(GetNormalizedDurability)
    DefineUFunction(GetFilterCategory)
    DefineUFunction(GetActiveAlterations)

public:
    UFortItemDefinition* GetItemDefinitionBP() const;
    int32_t GetLevel() const;
    // The item's perks that work (4.2 0xD31D00): its alteration slots whose MinRequiredLevel the item's level (or
    // PretendLevel, from 0) reaches and whose MinHostItemRarity its rarity does. Empty without the function.
    TArray<UObject*> GetActiveAlterations(int32_t PretendLevel = -1) const;
    bool HasDurability() const;
    float GetNormalizedDurability() const;
    EFortInventoryFilter GetFilterCategory() const;
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
