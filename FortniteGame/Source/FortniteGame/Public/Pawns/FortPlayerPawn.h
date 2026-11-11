#pragma once

#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Pawns/FortPawn.h"
#include "UObject/WeakObjectPtr.h"

class AController;
class AFortPickup;
class UFortHero;

class AFortPlayerPawn : public AFortPawn
{
    DefineUnrealClass(AFortPlayerPawn, FortniteGame)
    DefineUFunction(ServerReviveFromDBNO)
    DefineUFunction(ServerHandlePickup)
    DefineUFunction(IsUsingPersonalVehicle)

public:
    DefineUProperty(FGuid, LastEquippedWeaponGUID)
    // A DBNO revive applies one of these, healing to SetByCallerReviveHealth, then sends EventReviveTag, which plays the
    // getting-up ability (Battle Royale: GAB_AthenaDBNORevive on Event.RevivedAthena).
    DefineUProperty(UClass*, SelfReviveGameplayEffect)
    DefineUProperty(UClass*, TeammateReviveGameplayEffect)
    DefineUStructProperty(FScalableFloat, SetByCallerReviveHealth)
    DefineUProperty(FGameplayTag, EventReviveTag)
    // The hero the pawn plays (set with the controller's hero; not replicated).
    DefineUProperty(TWeakObjectPtr<UFortHero>, Hero)
    // EFortPawnStasisMode: the pawn may not act while it is anything but None.
    DefineUProperty(uint8_t, StasisMode)
    // From 3.5: set while the player's controller takes this pawn back from the remote-controlled pawn they flew (the
    // Guided Missile's; AFortRemoteControlledPawnAthena, 5.41 0x1234EA0 around its Possess). The pawn isn't new to the
    // player then: AFortPlayerPawnAthena::PossessedBy (5.41 0xC63200) skips the game mode's effects while it is set.
    DefineUProperty(bool, bBeingRepossessed)

    // Epic's getter (5.41 0x11820E0). False where the build has no remote-controlled pawns.
    bool IsBeingRepossessed() const { return Member_bBeingRepossessed.IsAvailable() && bBeingRepossessed; }

    void ResetGameplayAttributes();
    bool EquipLastWeapon();
    // 3.0 and later: whether the pawn rides its personal vehicle (movement style PersonalVehicle, which the pawn sets while
    // it has Gameplay.Action.Player.PersonalVehicle). False where the build has no such function.
    bool IsUsingPersonalVehicle() const;
    // A long interaction's animation (holding interact on a container or a downed teammate), which the controller's
    // long-interaction start asks of its pawn after OnStartLongInteract unless that skips it: the pawn's virtual (5.30
    // AFortPlayerPawnAthena slot 0x1B8, 0x117EAE0; 5.40 slot 0x1C4, 0x118B8E0) activates the abilities tagged
    // Gameplay.Action.Player.InteractSearch, which GAS_DefaultPlayer's GA_DefaultPlayer_InteractSearch is: its Search
    // montage (an intro, then a loop), played through the ability system and so replicated to every client. 21.40 calls
    // the pair StartItemInteractionAbility and StopItemInteractionAbility and picks the tag by an EItemInteractionType
    // (GetItemInteractAbilityTag); these builds have only the search. False when nothing activated.
    bool StartItemInteractionAbility();
    // Its end (the next slot: 5.30 0x117ECE0, 5.40 0x118B9E0), when the interaction completes or is let go:
    // CancelAbilities with that tag, which stops the montage.
    void StopItemInteractionAbility();
    // 1.8's revive (0x7FF160), restored where the build strips ServerReviveFromDBNO_Implementation (1.8.1).
    void ServerReviveFromDBNO_Implementation(AController* EventInstigator);
    // The pickup RPC, called as the game calls it (through ProcessEvent, which runs it in place on the server).
    void ServerHandlePickup(AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
    // 1.8.1's pickup RPC (0x80A2C0, with AFortPlayerPawnAthena's 0x4349E0), restored where the build strips
    // ServerHandlePickup_Implementation (1.8.2).
    void ServerHandlePickup_Implementation(AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);

    static ptrdiff_t LastEquippedWeaponGUIDOffset();
    static ptrdiff_t StasisModeOffset() { return Member_StasisMode.GetOffsetChecked(); }
    static void HookVTables();
    static void HookPickupVTables();

private:
    void ReviveFromDBNO(AController* EventInstigator);

    static void ServerReviveFromDBNO_ImplementationHK(AFortPlayerPawn* This, AController* EventInstigator);
    static void ServerHandlePickup_ImplementationHK(AFortPlayerPawn* This, AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
};
