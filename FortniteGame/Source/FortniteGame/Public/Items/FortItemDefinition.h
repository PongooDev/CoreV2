#pragma once

#include "Containers/UnrealString.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "StructArray.h"
#include "UObject/AssetPtr.h"
#include "UObject/Object.h"

class AFortPlayerController;
struct FFortItemEntry;
class IFortInventoryOwnerInterface;
class UFortItem;
class UFortWorldItem;

class EFortInventoryFilter
{
    DefineUnrealEnum(EFortInventoryFilter, FortniteGame)
    DefineUEnumValue(WeaponMelee)
    DefineUEnumValue(WeaponRanged)
    DefineUEnumValue(Ammo)
    DefineUEnumValue(Traps)
    DefineUEnumValue(Consumables)
    DefineUEnumValue(Ingredients)
    DefineUEnumValue(Gadget)
    DefineUEnumValue(Decorations)
    DefineUEnumValue(Badges)
    DefineUEnumValue(Heroes)
    DefineUEnumValue(LeadSurvivors)
    DefineUEnumValue(Survivors)
    DefineUEnumValue(Defenders)
    DefineUEnumValue(Resources)
    DefineUEnumValue(ConversionControl)
    DefineUEnumValue(Invisible)
    DefineUEnumValue(Max_None)
};

class EFortItemType
{
    DefineUnrealEnum(EFortItemType, FortniteGame)
    DefineUEnumValue(WorldItem)
    DefineUEnumValue(Ammo)
    DefineUEnumValue(Badge)
    DefineUEnumValue(BackpackPickup)
    DefineUEnumValue(BuildingPiece)
    DefineUEnumValue(CharacterPart)
    DefineUEnumValue(Consumable) // 1.8.1 and later; resolves to Reflection::InvalidEnumValue before that
    DefineUEnumValue(Deco)
    DefineUEnumValue(EditTool)
    DefineUEnumValue(Ingredient)
    DefineUEnumValue(Food)
    DefineUEnumValue(Gadget)
    DefineUEnumValue(HomebaseGadget)
    DefineUEnumValue(HeroAbility)
    DefineUEnumValue(MissionItem)
    DefineUEnumValue(Trap)
    DefineUEnumValue(Weapon)
    DefineUEnumValue(WeaponMelee)
    DefineUEnumValue(WeaponRanged)
    DefineUEnumValue(WeaponHarvest)
    DefineUEnumValue(WorldResource)
    DefineUEnumValue(AccountItem)
    DefineUEnumValue(AccountResource)
    DefineUEnumValue(CollectedResource)
    DefineUEnumValue(Alteration)
    DefineUEnumValue(CardPack)
    DefineUEnumValue(CharacterCosmetic)
    DefineUEnumValue(Currency)
    DefineUEnumValue(Hero)
    DefineUEnumValue(Schematic)
    DefineUEnumValue(Worker)
    DefineUEnumValue(Token)
    DefineUEnumValue(DailyRewardScheduleToken)
    DefineUEnumValue(CodeToken)
    DefineUEnumValue(Buff)
    DefineUEnumValue(BuffCredit)
    DefineUEnumValue(Quest)
    DefineUEnumValue(GameplayModifier)
    DefineUEnumValue(Outpost)
    DefineUEnumValue(HomebaseNode)
    DefineUEnumValue(Defender)
    DefineUEnumValue(ConversionControl)
    DefineUEnumValue(DeployableBaseCloudSave)
    DefineUEnumValue(ConsumableAccountItem)
    DefineUEnumValue(Quota)
    DefineUEnumValue(Expedition)
    DefineUEnumValue(HomebaseBannerIcon)
    DefineUEnumValue(HomebaseBannerColor)
    DefineUEnumValue(SpecialItem)
    DefineUEnumValue(Emote)
    DefineUEnumValue(Stack)
    DefineUEnumValue(CollectionBookPage)
    DefineUEnumValue(Profile)
    DefineUEnumValue(Max_None)
};

class UFortItemDefinition : public UObject
{
    DefineUnrealClass(UFortItemDefinition, FortniteGame)

public:
    DefineUProperty(EFortItemType, ItemType)
    DefineUProperty(int32_t, MaxStackSize)
    DefineBitfieldUProperty(bAllowMultipleStacks)
    DefineBitfieldUProperty(bInventorySizeLimited)
    DefineUProperty(FGameplayTagContainer, GameplayTags)
    DefineUProperty(bool, bCalculateRarityFromQualityAndTier)
    DefineUProperty(uint8_t, Quality)
    DefineUProperty(uint8_t, Tier)
    DefineUProperty(uint8_t, Rarity)
    DefineUFunction(CreateTemporaryItemInstanceBP)
    DefineUFunction(GetPersistentName)

    UFortItem* CreateTemporaryItemInstanceBP(int32_t Count, int32_t Level) const;
    FString GetPersistentName() const;
    uint8_t GetRarity() const;
};

class UFortWorldItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UFortWorldItemDefinition, FortniteGame)

public:
    DefineBitfieldUProperty(bCanBeDropped)
    DefineBitfieldUProperty(bCanBeDepositedInStorageVault)
    DefineUProperty(int32_t, PreferredQuickbarSlot)
    DefineUStructProperty(FDataTableCategoryHandle, LootLevelData)
    DefineUProperty(uint8_t, AccumulatingStatType)
    DefineUFunction(GetMaxDurability)

    const FDataTableCategoryHandle* GetLootLevelData() const { return &LootLevelData; }

    bool ServerExecute(const UFortItem* Item, AFortPlayerController* Controller);
    float GetMaxDurability(int32_t ItemLevel) const;
    void GetDisassemblyResults(TStructArray<FFortItemEntry>& OutResults, float NormalizedDurability) const;
};

class UFortNeverPersistItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortNeverPersistItemDefinition, FortniteGame)

public:
    DefineBitfieldUProperty(bAccumulateOnPlayerState)

    void OnItemCountAdded(IFortInventoryOwnerInterface* InventoryOwner, int32_t Count, bool bUnused);
    void OnItemCountRemoved(IFortInventoryOwnerInterface* InventoryOwner, int32_t Count);

    static void HookVTables();

private:
    void AccumulateOnPlayerState(IFortInventoryOwnerInterface* InventoryOwner, int32_t Delta);

    static inline void (*OnItemCountAddedOG)(UFortNeverPersistItemDefinition* This, IFortInventoryOwnerInterface* InventoryOwner, int32_t Count, bool bUnused) = nullptr;
    static inline void (*OnItemCountRemovedOG)(UFortNeverPersistItemDefinition* This, IFortInventoryOwnerInterface* InventoryOwner, int32_t Count) = nullptr;
    static void OnItemCountAddedHK(UFortNeverPersistItemDefinition* This, IFortInventoryOwnerInterface* InventoryOwner, int32_t Count, bool bUnused);
    static void OnItemCountRemovedHK(UFortNeverPersistItemDefinition* This, IFortInventoryOwnerInterface* InventoryOwner, int32_t Count);
};

class UFortGadgetItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortGadgetItemDefinition, FortniteGame)
};

struct FFortBaseWeaponStats
{
    DefineUnrealStruct(FFortBaseWeaponStats, FortniteGame)
    DefineUProperty(int32_t, ClipSize)
};

class UFortWeaponItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortWeaponItemDefinition, FortniteGame)

public:
    DefineUStructProperty(FDataTableRowHandle, WeaponStatHandle)
    DefineUProperty(TAssetPtr<UFortWorldItemDefinition>, AmmoData)

    // The magazine of the weapon's stats row (FFortBaseWeaponStats::ClipSize), 0 without one.
    int32_t GetClipSize() const;
};

class UFortAmmoItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortAmmoItemDefinition, FortniteGame)

public:
    DefineUProperty(float, RegenCooldown)

    static ptrdiff_t RegenCooldownOffset();
};
