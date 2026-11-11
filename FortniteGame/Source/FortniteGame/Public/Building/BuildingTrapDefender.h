#pragma once

#include "Building/BuildingTrap.h"
#include "UObject/Stack.h"

#include <cstdint>

class AFortAIPawn;
class UWorld;
class UFortWorldItem;

// The defender post (Trap_Floor_Defender_Master_C; an ABuildingTrapFloor). Its blueprint's DeployDefender, which
// AFortPlayerController::ServerDeployDefender_Implementation calls with the weapon and ammo count the player chose,
// spawns the defender (SpawnDefenderfromItem) and arms it through SupplyDefender; swapping in another defender takes
// the weapon from the old one first (TakeWeaponAwayFromDefender). Both are server code the client builds strip: their
// execs step the parameters and call an empty function (3.0's SupplyDefender 0x1049E80 calls a moved-code `ret`,
// 0x25A4AD0; its TakeWeaponAwayFromDefender shares AActor::execMakeMIDForMaterial, 0xFA82D0, which calls the
// return-zero stub; 2.5's SupplyDefender 0xF7A2F0 calls nullsub_2), so every defender came out with no weapon and no
// ammo (CoreV2 #229).
class ABuildingTrapDefender : public ABuildingTrap
{
    DefineUnrealClass(ABuildingTrapDefender, FortniteGame)
    DefineUFunction(SupplyDefender)
    DefineUFunction(TakeWeaponAwayFromDefender)

public:
    DefineUProperty(AFortAIPawn*, DefenderPawn)

    // The defender gets WeaponToGive (moved out of its player's inventory; one it already holds stays) in its hands, and
    // AmmoQuantity of that weapon's ammo, the count the post's ammo slider sets: more comes from the player, the rest goes
    // back to them. A weapon or ammo it held before and no longer uses goes back to the player.
    void SupplyDefender(const UFortWorldItem* WeaponToGive, int32_t AmmoQuantity);
    // Takes WeaponToTakeAway out of the defender's inventory into its player's and returns the player's item, for the
    // defender that takes over the post. An item the defender doesn't hold comes back as it is.
    UFortWorldItem* TakeWeaponAwayFromDefender(UFortWorldItem* WeaponToTakeAway);

    DECLARE_FUNCTION(execSupplyDefender);
    DECLARE_FUNCTION(execTakeWeaponAwayFromDefender);

    static void HookNatives();

private:
    // A supplied defender gives its weapon and ammo back once it leaves its post: unsummoned, replaced, killed, or the
    // post gone. The post's blueprint clears its DefenderPawn when it lets the defender go (its ClearCurrentDefender:
    // AFortGameStateZone::OnDefenderRemoved, then SetDefenderPawn(None), before the defender teleports out; and from the
    // defender's OnEndPlay, which it binds), so that is what is looked for, on a timer (QueueReturnCheck) while supplied
    // defenders stand. Kept as a check: everything the post runs at that moment is intact (OnDefenderRemoved too, 5.30
    // 0xE83C00), so there is nothing stripped to restore there, and the defender's OnEndPlay is a dynamic delegate,
    // which only a blueprint function can be bound to.
    static void WatchDefender(ABuildingTrapDefender* Trap, AFortAIPawn* Defender);
    static void QueueReturnCheck(UWorld* World);
    static void ReturnLeftDefendersItems();
};
