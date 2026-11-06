#pragma once

#include "AI/FortAIEncounterInfo.h"
#include "GameplayTagContainer.h"
#include "Math/Vector.h"
#include "Misc/Guid.h"
#include "Pawns/FortPawn.h"

class ABuildingTrapDefender;
class AFortInventory;
class AFortPlayerController;
class AFortPlayerStateZone;
class IFortInventoryOwnerInterface;
class UFortWorldItem;

class AFortAIPawn : public AFortPawn
{
    DefineUnrealClass(AFortAIPawn, FortniteGame)

public:
    // A defender's: the player who placed it (its post's summoner) and the post it stands at.
    DefineUProperty(AFortPlayerStateZone*, PlayerManager)
    DefineUProperty(ABuildingTrapDefender*, DefenderTrap)
    // The encounter that spawned it, and which of that encounter's spawn groups and which of the group's enemies it is.
    DefineUProperty(UFortAIEncounterInfo*, EncounterInfo)
    DefineUProperty(FGuid, SpawnGroupGuid)
    DefineUProperty(int32_t, EnemyIndexInSpawnGroup)
    DefineBitfieldUProperty(bUsesDBNO)
    // 4.0: set once the pawn has dropped its loot.
    DefineBitfieldUProperty(bHasDroppedLoot)
    // 4.0: its loot leaves within this many degrees of straight up, at this speed (AFortAIPawn's: 15 and 950).
    DefineUProperty(float, LootDropConeHalfAngle)
    DefineUProperty(float, LootDropSpeed)

    IFortInventoryOwnerInterface* GetInventoryOwner();
    AFortInventory* GetWorldInventory();
    bool RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);

    // The controller of the player who manages this defender (PlayerManager's owner), null once they have left.
    AFortPlayerController* GetPlayerManagerController();
    // A defender leaving its post gives its weapons and ammo back to the player who manages it; with that player gone,
    // they drop where it stands.
    void ReturnInventoryToPlayerManager();
    // Count of Item (one in this pawn's inventory) to that player, or to the ground; returns the player's item it became.
    UFortWorldItem* ReturnItemToPlayerManager(UFortWorldItem* Item, int32_t Count);

    // 4.0 (#289, #290): Save the World's enemy loot drops, which the client exe strips. A pawn the director spawned rolls
    // its loot drop data (UFortAIEncounterInfo::GetAIPawnLootDropData) as it dies: its world item chance drops pickups
    // from its world item tier group (resources, ammo, crafting ingredients and traps), and its account item chance
    // drops one from its account item tier group (a Mini-Boss's item cache). None for a death the director's
    // LootDropDenialQuery matches the damage tags of. Nothing on builds without loot drop rows.
    void DropLoot(const FFortAIPawnLootDropData& LootDropData, const FGameplayTagContainer& DamageTags);

    static void HookVTables();

private:
    // The pawn's death handler (4.0 0xEEBAC0, AFortAIPawn's slot 0x123): after a death that isn't a knock-down, the pawn
    // drops its loot.
    static void HandleDeathHK(AFortAIPawn* This, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, const void* HitInfo, void* Arg5);
    static inline void (*HandleDeathOG)(AFortAIPawn* This, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, const void* HitInfo, void* Arg5) = nullptr;
    static void HookDeathVTables();

    static bool RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars);

    static inline ptrdiff_t InventoryOwnerInterfaceOffset = -1;
};
