#pragma once

#include "Containers/UnrealString.h"
#include "UObject/Object.h"

class AFortPlayerController;
class UFortItem;
class UFortWorldItem;

enum class EFortItemType : uint8_t
{
    WorldItem,
    Ammo,
    Badge,
    BackpackPickup,
    BuildingPiece,
    CharacterPart,
    Deco,
    EditTool,
    Ingredient,
    Food,
    Gadget,
    HomebaseGadget,
    HeroAbility,
    MissionItem,
    Trap,
    Weapon,
    WeaponMelee,
    WeaponRanged,
    WeaponHarvest,
    WorldResource,
    AccountItem,
    AccountResource,
    CollectedResource,
    Alteration,
    CardPack,
    CharacterCosmetic,
    Currency,
    Hero,
    Schematic,
    Worker,
    Token,
    DailyRewardScheduleToken,
    CodeToken,
    Buff,
    BuffCredit,
    Quest,
    GameplayModifier,
    Outpost,
    HomebaseNode,
    Defender,
    ConversionControl,
    DeployableBaseCloudSave,
    ConsumableAccountItem,
    Quota,
    Expedition,
    HomebaseBannerIcon,
    HomebaseBannerColor,
    SpecialItem,
    Emote,
    Stack,
    CollectionBookPage,
    Profile,
    Max_None,
};

class UFortItemDefinition : public UObject
{
    DefineUnrealClass(UFortItemDefinition, FortniteGame)

public:
    DefineUProperty(uint8_t, ItemType)
    DefineUProperty(int32_t, MaxStackSize)
    DefineUFunction(CreateTemporaryItemInstanceBP)
    DefineUFunction(GetPersistentName)
    DefineUFunction(GetFilterCategory)

    UFortItem* CreateTemporaryItemInstanceBP(int32_t Count, int32_t Level) const;
    FString GetPersistentName() const;
    uint8_t GetFilterCategory() const;
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
