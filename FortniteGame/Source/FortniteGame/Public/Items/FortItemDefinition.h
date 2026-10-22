#pragma once

#include "Containers/UnrealString.h"
#include "UObject/Object.h"

class AFortPlayerController;
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
    DefineUFunction(CreateTemporaryItemInstanceBP)
    DefineUFunction(GetPersistentName)
    DefineUFunction(GetFilterCategory)

    UFortItem* CreateTemporaryItemInstanceBP(int32_t Count, int32_t Level) const;
    FString GetPersistentName() const;
    EFortInventoryFilter GetFilterCategory() const;
};

class UFortWorldItemDefinition : public UFortItemDefinition
{
    DefineUnrealClass(UFortWorldItemDefinition, FortniteGame)

public:
    DefineBitfieldUProperty(bCanBeDropped)
    DefineBitfieldUProperty(bCanBeDepositedInStorageVault)
    DefineUProperty(int32_t, PreferredQuickbarSlot)
    DefineUFunction(GetMaxDurability)

    bool ServerExecute(const UFortItem* Item, AFortPlayerController* Controller);
    float GetMaxDurability(int32_t ItemLevel) const;
};

class UFortGadgetItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortGadgetItemDefinition, FortniteGame)
};

class UFortWeaponItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortWeaponItemDefinition, FortniteGame)
};

class UFortAmmoItemDefinition : public UFortWorldItemDefinition
{
    DefineUnrealClass(UFortAmmoItemDefinition, FortniteGame)

public:
    DefineUProperty(float, RegenCooldown)

    static ptrdiff_t RegenCooldownOffset();
};
