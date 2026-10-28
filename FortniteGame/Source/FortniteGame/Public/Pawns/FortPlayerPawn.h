#pragma once

#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Pawns/FortPawn.h"

class AController;

class AFortPlayerPawn : public AFortPawn
{
    DefineUnrealClass(AFortPlayerPawn, FortniteGame)
    DefineUFunction(ServerReviveFromDBNO)

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

    static ptrdiff_t LastEquippedWeaponGUIDOffset();
    static void HookVTables();

private:
    void ReviveFromDBNO(AController* EventInstigator);

    static void ServerReviveFromDBNO_ImplementationHK(AFortPlayerPawn* This, AController* EventInstigator);
};
