#pragma once

#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Pawns/FortPawn.h"

class AController;
class AFortPickup;

class AFortPlayerPawn : public AFortPawn
{
    DefineUnrealClass(AFortPlayerPawn, FortniteGame)
    DefineUFunction(ServerReviveFromDBNO)
    DefineUFunction(ServerHandlePickup)

public:
    DefineUProperty(FGuid, LastEquippedWeaponGUID)
    // A DBNO revive applies one of these, healing to SetByCallerReviveHealth, then sends EventReviveTag, which plays the
    // getting-up ability (Battle Royale: GAB_AthenaDBNORevive on Event.RevivedAthena).
    DefineUProperty(UClass*, SelfReviveGameplayEffect)
    DefineUProperty(UClass*, TeammateReviveGameplayEffect)
    DefineUStructProperty(FScalableFloat, SetByCallerReviveHealth)
    DefineUProperty(FGameplayTag, EventReviveTag)

    void ResetGameplayAttributes();
    bool EquipLastWeapon();
    // 1.8's revive (0x7FF160), restored where the build strips ServerReviveFromDBNO_Implementation (1.8.1).
    void ServerReviveFromDBNO_Implementation(AController* EventInstigator);
    // The pickup RPC, called as the game calls it (through ProcessEvent, which runs it in place on the server).
    void ServerHandlePickup(AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
    // 1.8.1's pickup RPC (0x80A2C0, with AFortPlayerPawnAthena's 0x4349E0), restored where the build strips
    // ServerHandlePickup_Implementation (1.8.2).
    void ServerHandlePickup_Implementation(AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);

    static ptrdiff_t LastEquippedWeaponGUIDOffset();
    static void HookVTables();
    static void HookPickupVTables();

private:
    void ReviveFromDBNO(AController* EventInstigator);

    static void ServerReviveFromDBNO_ImplementationHK(AFortPlayerPawn* This, AController* EventInstigator);
    static void ServerHandlePickup_ImplementationHK(AFortPlayerPawn* This, AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
};
