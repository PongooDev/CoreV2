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
class UGameplayAbility;

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
    // Whether the build reflects what GetRarity reads.
    static bool HasRarity() { return Member_bCalculateRarityFromQualityAndTier.IsAvailable() && Member_Quality.IsAvailable() && Member_Tier.IsAvailable() && Member_Rarity.IsAvailable(); }
};

class UFortWorldItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UFortWorldItemDefinition, FortniteGame)

public:
    DefineBitfieldUProperty(bCanBeDropped)
    DefineBitfieldUProperty(bCanBeDepositedInStorageVault)
    DefineBitfieldUProperty(bPersistInInventoryWhenFinalStackEmpty)
    DefineBitfieldUProperty(bItemHasDurability)
    DefineUProperty(int32_t, PreferredQuickbarSlot)
    DefineUStructProperty(FDataTableCategoryHandle, LootLevelData)
    DefineUProperty(uint8_t, AccumulatingStatType)
    DefineUFunction(GetMaxDurability)

    const FDataTableCategoryHandle* GetLootLevelData() const { return &LootLevelData; }
    // 3.2 added the flag (3.3's Remote Explosives, Athena_C4, set it): the item's last stack stays at 0 instead of
    // leaving the inventory, as the C4 detonator does while thrown charges are out.
    bool ShouldPersistInInventoryWhenFinalStackEmpty() const { return Member_bPersistInInventoryWhenFinalStackEmpty.IsAvailable() && bPersistInInventoryWhenFinalStackEmpty; }
    // The item wears down (its entries carry a durability); false where the build doesn't reflect the flag.
    bool ItemHasDurability() const { return Member_bItemHasDurability.IsAvailable() && bItemHasDurability; }
    // The item leaves its carrier when they go down (5.40's The Getaway jewel, Athena_Bling_Pack). The server's drop is
    // stripped: AFortPlayerControllerAthena::DropItemsOnDBNO. False where the build doesn't reflect the flag.
    DefineBitfieldUProperty(bDropOnDBNO)
    bool ShouldDropOnDBNO() const { return Member_bDropOnDBNO.IsAvailable() && bDropOnDBNO; }

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

public:
    // Taking the gadget drops everything else its taker carries (4.1's Infinity Gauntlet; #297).
    DefineUProperty(bool, bDropAllOnEquip)
    static bool HasDropAllOnEquip() { return Member_bDropAllOnEquip.IsAvailable(); }
};

// An item used up as it is used. StaticClass is null on a build without the class.
class UFortConsumableItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortConsumableItemDefinition, FortniteGame)
};

// A Save the World item cache (4.0): a world item a Mini-Boss drops (Cache_EvolveTier_VR_Pickup_T01-T04,
// Cache_Schematic_R/VR/SR_Pickup) that the player keeps for the end of the mission, when the MCP gives its
// CardPackReward (a TSoftObjectPtr<UFortCardPackItemDefinition>). StaticClass is null on a build without the class.
class UFortItemCacheItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortItemCacheItemDefinition, FortniteGame)
};

// An emote or a dance (an account item): the montage it plays (TSoftObjectPtr<UAnimMontage>).
class UFortMontageItemDefinitionBase : public UFortItemDefinition
{
    DefineUnrealClass(UFortMontageItemDefinitionBase, FortniteGame)

public:
    DefineUProperty(TAssetPtr<UObject>, Animation)

    // -1 where the build has no such property.
    static ptrdiff_t AnimationOffset() { return Member_Animation.IsAvailable() ? Member_Animation.GetOffsetChecked() : -1; }
};

// A spray (4.0, Season 4; an AthenaDanceItemDefinition in the game): the decal the spray ability places. StaticClass is
// null on a build without the class.
class UAthenaSprayItemDefinition : public UFortMontageItemDefinitionBase
{
    DefineUnrealClass(UAthenaSprayItemDefinition, FortniteGame)
};

// A toy (5.0, Season 5: the basketball, golf ball and beach ball; a UAthenaDanceItemDefinition in the game): the ability
// the toy's emote plays (GAB_Toy_BasketballToss and the other GAB_ToyThrow_Base children) and the toy actor it throws.
// StaticClass is null on a build without the class.
class UAthenaToyItemDefinition : public UFortMontageItemDefinitionBase
{
    DefineUnrealClass(UAthenaToyItemDefinition, FortniteGame)

public:
    DefineUProperty(TAssetSubclassOf<UGameplayAbility>, ToySpawnAbility)
    static bool HasToySpawnAbility() { return Member_ToySpawnAbility.IsAvailable(); }
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
