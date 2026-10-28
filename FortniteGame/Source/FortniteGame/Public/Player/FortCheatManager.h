#pragma once

#include "Containers/UnrealString.h"
#include "GameFramework/CheatManager.h"
#include "UObject/NameTypes.h"
#include "UObject/Stack.h"

#include <cstdint>
#include <initializer_list>

class AFortPlayerController;
class UFortWorldItemDefinition;

// Fortnite's cheats ("cheat <command>" runs them on the server, AFortPlayerController::ServerCheat_Implementation).
// Shipping builds keep every exec function but compile many bodies out, so those cheats answered nothing and did
// nothing (#101). Core gives the stripped ones the server's body back; Core-Docs Cheats.md lists every command.
class UFortCheatManager : public UCheatManager
{
    DefineUnrealClass(UFortCheatManager, FortniteGame)
    DefineUFunction(GiveWeapon)
    DefineUFunction(GiveAmmo)
    DefineUFunction(GiveTrap)
    DefineUFunction(GiveGadget)
    DefineUFunction(GiveGadgets)
    DefineUFunction(GiveSpecificItem)
    DefineUFunction(GiveResources)
    DefineUFunction(GiveWood)
    DefineUFunction(GiveStone)
    DefineUFunction(GiveMetal)
    DefineUFunction(GiveWeapons)
    DefineUFunction(GiveAllMelee)
    DefineUFunction(GiveAllPistols)
    DefineUFunction(GiveAllRifles)
    DefineUFunction(GiveAllShotguns)
    DefineUFunction(GiveAllWeapons)
    DefineUFunction(GiveAllTraps)
    DefineUFunction(GiveAllConsumables)
    DefineUFunction(GiveAllIngredients)
    DefineUFunction(GiveUsefulThings)
    DefineUFunction(GiveCheatInventory)
    DefineUFunction(WannaGun)
    DefineUFunction(TeleportToLocation)
    DefineUFunction(SetSafeZoneLocationToPlayer)
    DefineUFunction(SetSafeZoneRadius)
    DefineUFunction(RespawnPlayerServer)

public:
    AFortPlayerController* GetPlayerController() const;

    // WeaponName: the item's name with or without its prefix ("Assault_Auto_Athena_R_Ore_T03" or "WID_..."), or part
    // of it. Count weapons at RequestedLevel, a ranged weapon with a full magazine.
    void GiveWeapon(const FString& WeaponName, int32_t RequestedLevel, int32_t Count);
    // "AthenaAmmoDataBulletsHeavy 100"; without an amount, a full stack.
    void GiveAmmo(const FString& AmmoNameAndAmount);
    void GiveTrap(const FString& TrapName, int32_t RequestedLevel, int32_t NumTrapsToGive);
    void GiveGadgets(const FString& GadgetName, int32_t Quantity, int32_t ItemLevel);
    // ItemPath: an item's object or package path ("/Game/Items/Ingredients/Ingredient_Planks"), or its name.
    void GiveSpecificItem(const FString& ItemPath, int32_t ItemAmount, int32_t ItemLevel);
    // Wood, stone and metal; without an amount, a full stack of each.
    void GiveResources(int32_t Amount);
    void GiveResource(UFortWorldItemDefinition* Resource, int32_t Amount);
    // One of every weapon with the gameplay tag or one under it (None: every weapon), the best rarity and tier of each.
    void GiveWeapons(FName Tag);
    void GiveAllTraps(int32_t NumItems);
    void GiveAllConsumables(int32_t NumItems);
    void GiveAllIngredients(int32_t NumIngredients);
    void GiveUsefulThings(int32_t NumOfUsefulThings);
    // The game data's CheatInventoryItems (1.7.2 and 1.8: the TestGod rifle).
    void GiveCheatInventory();
    // The WannaGun test gun (/Game/Items/Weapons/Ranged/WIP/WannaGun).
    void WannaGun();
    void TeleportToLocation(float X, float Y, float Z);
    // Battle Royale: the safe zones after the current one close in on the player.
    void SetSafeZoneLocationToPlayer();
    // Battle Royale: the storm circle has this radius until the next phase.
    void SetSafeZoneRadius(float NewRadius);
    void RespawnPlayerServer();

    // Server: the stripped cheats run Core's bodies.
    static void HookNatives();

    DECLARE_FUNCTION(execGiveWeapon);
    DECLARE_FUNCTION(execGiveAmmo);
    DECLARE_FUNCTION(execGiveTrap);
    DECLARE_FUNCTION(execGiveGadget);
    DECLARE_FUNCTION(execGiveGadgets);
    DECLARE_FUNCTION(execGiveSpecificItem);
    DECLARE_FUNCTION(execGiveResources);
    DECLARE_FUNCTION(execGiveWood);
    DECLARE_FUNCTION(execGiveStone);
    DECLARE_FUNCTION(execGiveMetal);
    DECLARE_FUNCTION(execGiveWeapons);
    DECLARE_FUNCTION(execGiveAllMelee);
    DECLARE_FUNCTION(execGiveAllPistols);
    DECLARE_FUNCTION(execGiveAllRifles);
    DECLARE_FUNCTION(execGiveAllShotguns);
    DECLARE_FUNCTION(execGiveAllWeapons);
    DECLARE_FUNCTION(execGiveAllTraps);
    DECLARE_FUNCTION(execGiveAllConsumables);
    DECLARE_FUNCTION(execGiveAllIngredients);
    DECLARE_FUNCTION(execGiveUsefulThings);
    DECLARE_FUNCTION(execGiveCheatInventory);
    DECLARE_FUNCTION(execWannaGun);
    DECLARE_FUNCTION(execTeleportToLocation);
    DECLARE_FUNCTION(execSetSafeZoneLocationToPlayer);
    DECLARE_FUNCTION(execSetSafeZoneRadius);
    DECLARE_FUNCTION(execRespawnPlayerServer);

private:
    void Reply(const FString& Message) const;
    // The player, when they have an inventory to give items to.
    AFortPlayerController* GetReceiver() const;
    // Gives the first item of the asset manager's types that the name finds, and says which.
    void GiveNamedItem(std::initializer_list<const wchar_t*> PrimaryAssetTypes, const FString& Name, int32_t Count, int32_t Level);
};
