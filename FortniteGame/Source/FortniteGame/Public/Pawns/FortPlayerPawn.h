#pragma once

#include "GameplayEffect.h"
#include "GameplayTagContainer.h"
#include "Pawns/FortPawn.h"
#include "UObject/Stack.h"
#include "UObject/WeakObjectPtr.h"

class AController;
class AFortPickup;
class UFortHero;

// From 6.00: what AFortPlayerPawn::SetInVortex gives a pawn in a vortex (FVortexParams, the same 16 bytes on 6.00 to
// 6.20). In a vortex the skydive's and the glider's gravity (UFortMovementComp_Character, 6.10 0x10431D0) is the vortex's
// own control params' (a glider climbs), and below GravityFloorAltitude + GravityFloorWidth it blends towards
// GravityFloorGravityScalar.
struct FVortexParams
{
    float GravityFloorAltitude;
    float GravityFloorWidth;
    float GravityFloorGravityScalar;
    float GravityFloorTerminalVelocity;
};

// The pawn's zipline ride (AFortPlayerPawn::ZiplineState), as 7.20 (CL-4727874) has it (0x28 bytes): the zipline, whether
// the pawn rides it and whether it jumped off, the counter the owning client raises with each change it sends
// (ServerSendZiplineState), where on the line it hangs, and two times the client keeps for itself (RepSkip, so the RPC
// doesn't carry them): when the ride began and when a jump ended it.
struct FZiplinePawnState
{
    DefineUnrealStruct(FZiplinePawnState, FortniteGame)

public:
    DefineUProperty(bool, bIsZiplining)
    DefineUProperty(bool, bJumped)
    DefineUProperty(int32_t, AuthoritativeValue)
    DefineUProperty(float, TimeZipliningBegan)
    DefineUProperty(float, TimeZipliningEndedFromJump)
    static bool HasRideTimes() { return Member_TimeZipliningBegan.IsAvailable() && Member_TimeZipliningEndedFromJump.IsAvailable(); }
};

class AFortPlayerPawn : public AFortPawn
{
    DefineUnrealClass(AFortPlayerPawn, FortniteGame)
    DefineUFunction(ServerReviveFromDBNO)
    DefineUFunction(ManualReviveFromDBNO)
    DefineUFunction(ServerSendZiplineState)
    DefineUFunction(OnRep_ZiplineState)
    DefineUFunction(ServerHandlePickup)
    DefineUFunction(ServerHandlePickupWithSwap)
    // 8.50 (CL-6058028).
    DefineUFunction(ServerHandlePickupWithRequestedSwap)
    DefineUFunction(IsUsingPersonalVehicle)
    DefineUFunction(PushAnimBPOverride)
    DefineUFunction(PopAnimBPOverride)
    DefineUFunction(PushFootstepBankOverride)
    DefineUFunction(PopFootstepBankOverride)
    DefineUFunction(OnRep_AnimBPOverride)
    DefineUFunction(OnRep_FootstepBankOverride)

public:
    DefineUProperty(FGuid, LastEquippedWeaponGUID)
    // 7.10's name for it (CL-4667333 +0x14F8): gadgets count as the last equipped item too.
    DefineUProperty(FGuid, LastEquippedWeaponOrGadgetGUID)
    // Only its offset: 8.40's unreflected LastEquippedWeaponOrGadgetGUID sits 0x20 bytes before it.
    DefineUProperty(UObject*, VehicleInputComponent)
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
    // player then: AFortPlayerPawnAthena::PossessedBy (5.41 0xC63200) skips the game mode's effects while it is set. A
    // single bit from 7.40 (CL-5046157), as is bIsInVortex.
    DefineBitfieldUProperty(bBeingRepossessed)
    // From 6.00: whether the pawn is in a vortex (the cube's, under the floating island: BGA_DudeBro_Area_Effect's funnel
    // gives an entering pawn GE_DudeBro_Area_Effect, whose GA_DudeBro_Vortex calls SetInVortex, 6.10 0x11FE900, the
    // pawn's virtual), and the gravity floor its movement has there. The funnel's end overlap removes the effect, which
    // cancels the ability, and its end calls SetInVortex(false).
    DefineBitfieldUProperty(bIsInVortex)
    DefineUProperty(FVortexParams, VortexParams)
    // From 6.00: the anim Blueprint and footstep bank a held gadget gives the pawn (replicated; their OnReps swap the
    // mesh's anim class, keeping OriginalAnimBP, and the footsteps' bank, on the server and each client).
    DefineUProperty(UClass*, AnimBPOverride)
    DefineUProperty(UObject*, FootstepBankOverride)

    // Epic's getter (5.41 0x11820E0). False where the build has no remote-controlled pawns.
    bool IsBeingRepossessed() const { return Member_bBeingRepossessed.IsAvailable() && bBeingRepossessed; }
    // Epic's getter (AFortPlayerPawn::IsInVortex, 6.10 0x158E8C0's native). False before 6.00, which has no vortex.
    bool IsInVortex() const { return Member_bIsInVortex.IsAvailable() && bIsInVortex; }
    static bool HasVortexParams() { return Member_VortexParams.IsAvailable(); }

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
    // The pickup that takes the place of one of the player's items (Swap, the item's guid), where the build strips
    // ServerHandlePickupWithSwap_Implementation (7.30 and 7.40: the shared empty function). 7.40's client sends it when the
    // player holds interact on a pickup ("swap the item in the currently selected slot"), the only way the server learns
    // which item to give up there, as 7.40 keeps the quickbars on the client.
    void ServerHandlePickupWithSwap_Implementation(AFortPickup* Pickup, const FGuid& Swap, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
    // 8.50's hold to swap (CL-6058028: the shared empty function, slot 0x1B8). The pickup's interact (0x1462D10) sends it
    // in place of ServerHandlePickup while the controller's bTryPickupSwap is set (interact held), Swap being the held
    // weapon's ItemEntryGuid: the held item goes even when the backpack has room.
    void ServerHandlePickupWithRequestedSwap_Implementation(AFortPickup* Pickup, const FGuid& Swap, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
    // Whether Pickup was taken with ServerHandlePickupWithRequestedSwap; asked once, when it arrives
    // (AFortPickup::GivePickupToPlayer).
    static bool ConsumeRequestedSwap(const AFortPickup* Pickup);

    static ptrdiff_t LastEquippedWeaponGUIDOffset();
    static ptrdiff_t StasisModeOffset() { return Member_StasisMode.GetOffsetChecked(); }
    // From 6.00 (the gadgets' anim and footstep overrides: Shadow Stones, 6.21's Butterfly ghost): the pawn's priority
    // stacks of overrides, the highest on top (FortPlayerPawn.cpp). UFortGadgetItemDefinition::ApplyGadgetData pushes a
    // held gadget's, RemoveGadgetData pops them. The client builds strip all four.
    void PushAnimBPOverride(int32_t Priority, UClass* InAnimBPOverride);
    void PopAnimBPOverride(int32_t Priority, UClass* InAnimBPOverride);
    void PushFootstepBankOverride(int32_t Priority, UObject* InFootstepBankOverride);
    void PopFootstepBankOverride(int32_t Priority, UObject* InFootstepBankOverride);

    // From 7.00 (ziplines): the ride's state, replicated (OnRep_ZiplineState sets the zipline movement), and the jump off
    // it, which the server launches as the client does. The jump's two are curve table rows (7.10 +0x1268, 7.20 +0x1270),
    // not scalable floats; 7.00 has neither.
    DefineUStructProperty(FZiplinePawnState, ZiplineState)
    DefineUStructProperty(FCurveTableRowHandle, ZiplineJumpDampening)
    DefineUStructProperty(FCurveTableRowHandle, ZiplineJumpStrength)
    static bool HasZiplineJump() { return Member_ZiplineJumpDampening.IsAvailable() && Member_ZiplineJumpStrength.IsAvailable(); }
    // Restored (7.20 strips it, 7.10 0x129AAB0): the owning client's ride state, taken when newer than the server's.
    void ServerSendZiplineState_Implementation(const FZiplinePawnState& InZiplineState);

    static void HookVTables();
    static void HookPickupVTables();
    // ServerSendZiplineState_Implementation where the build strips it (7.20).
    static void HookZiplineVTables();
    // The four override natives and the gadget data's calls to them, where the build strips them.
    static void HookNatives();

    // 8.30+: the server gets a downed player up (GenMission_TieredWaves' DoTeamRespawn at a wave's end, which 9.00's
    // Wargames turn on). Stripped from the build's server code.
    void ManualReviveFromDBNO();
    DECLARE_FUNCTION(execManualReviveFromDBNO);
    static void HookManualReviveNative();
    // Both DLLs, before the detours are enabled: TryToAutoPickupWeapon's guard where the tick reaches it (7.40, 8.00).
    static void Hook();

    // The owning client's walk-over pickup (7.40 0x13DF6D0): picks the overlapped pickup up through the local player's
    // interact, which sends ServerHandlePickup. Core's guard returns where there's no local player controller.
    void TryToAutoPickupWeapon(AFortPickup* Pickup);

private:
    static void TryToAutoPickupWeaponHK(AFortPlayerPawn* This, AFortPickup* Pickup);
    static inline void (*TryToAutoPickupWeaponOG)(AFortPlayerPawn* This, AFortPickup* Pickup) = nullptr;

    void ReviveFromDBNO(AController* EventInstigator);

    static void ServerReviveFromDBNO_ImplementationHK(AFortPlayerPawn* This, AController* EventInstigator);
    static void ServerSendZiplineState_ImplementationHK(AFortPlayerPawn* This, const FZiplinePawnState& InZiplineState);
    DECLARE_FUNCTION(execPushAnimBPOverride);
    DECLARE_FUNCTION(execPopAnimBPOverride);
    DECLARE_FUNCTION(execPushFootstepBankOverride);
    DECLARE_FUNCTION(execPopFootstepBankOverride);
    static void PushAnimBPOverrideHK(AFortPlayerPawn* This, int32_t Priority, UClass* InAnimBPOverride);
    static void PopAnimBPOverrideHK(AFortPlayerPawn* This, int32_t Priority, UClass* InAnimBPOverride);
    static void PushFootstepBankOverrideHK(AFortPlayerPawn* This, int32_t Priority, UObject* InFootstepBankOverride);
    static void PopFootstepBankOverrideHK(AFortPlayerPawn* This, int32_t Priority, UObject* InFootstepBankOverride);
    static void ServerHandlePickup_ImplementationHK(AFortPlayerPawn* This, AFortPickup* Pickup, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
    static void ServerHandlePickupWithSwap_ImplementationHK(AFortPlayerPawn* This, AFortPickup* Pickup, const FGuid& Swap, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
    static void ServerHandlePickupWithRequestedSwap_ImplementationHK(AFortPlayerPawn* This, AFortPickup* Pickup, const FGuid& Swap, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound);
};
