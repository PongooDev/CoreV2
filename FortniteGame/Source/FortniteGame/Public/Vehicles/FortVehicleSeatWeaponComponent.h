#pragma once

#include "Components/ActorComponent.h"
#include "Containers/Map.h"
#include "Reflection.h"
#include "StructArray.h"
#include "UObject/WeakObjectPtr.h"
#include "Weapons/FortWeapon.h"

class AActor;
class AFortPawn;
class IFortVehicleInterface;
class UFortWeaponItemDefinition;
class UFortWorldItem;

// One seat's weapon (9.00+). From 9.10 it ends in two unreflected weak pointers, VehicleGrantedWeaponItem and
// VehicleGrantedWeapon (21.40's names), which UnEquipVehicleWeapon (CL-6573057 0x1160F80) reads at +0x20 and +0x28,
// right after LastEquippedVehicleWeapon.
struct FWeaponSeatDefinition
{
    DefineUnrealStruct(FWeaponSeatDefinition, FortniteGame)

public:
    DefineUProperty(int32_t, SeatIndex)
    DefineUProperty(UFortWeaponItemDefinition*, VehicleWeapon)
    DefineUProperty(UFortWeaponItemDefinition*, VehicleWeaponOverride)
    DefineUProperty(UFortWeaponItemDefinition*, LastEquippedVehicleWeapon)

    TWeakObjectPtr<UFortWorldItem>& GetVehicleGrantedWeaponItem();
    TWeakObjectPtr<AFortWeapon>& GetVehicleGrantedWeapon();
    // 9.10+: the two weak pointers fill the struct after LastEquippedVehicleWeapon.
    static bool HasGrantedWeapon();
};

// The host vehicle a vehicle weapon replicates to its clients (9.10+), whose OnRep_MountedWeaponInfoRepped hooks the
// weapon up to the vehicle's seat.
struct FMountedWeaponInfoRepped
{
    DefineUnrealStruct(FMountedWeaponInfoRepped, FortniteGame)

public:
    DefineUProperty(AActor*, HostVehicleCachedActor)
    DefineUProperty(int32_t, HostVehicleSeatIndexCached)
    static bool HasHostVehicle() { return Member_HostVehicleCachedActor.IsAvailable() && Member_HostVehicleSeatIndexCached.IsAvailable(); }
};

// A weapon fired from a vehicle seat (the Pirate Cannon's B_MountedCannonAthena and B_MountedCannonAthena_InCannon).
class AFortWeaponRangedForVehicle : public AFortWeaponRanged
{
    DefineUnrealClass(AFortWeaponRangedForVehicle, FortniteGame)
    DefineUFunction(OnRep_MountedWeaponInfoRepped)

public:
    DefineUStructProperty(FMountedWeaponInfoRepped, MountedWeaponInfoRepped)
};

class AFortWeaponRangedDualForVehicle : public AFortWeaponRangedDual
{
    DefineUnrealClass(AFortWeaponRangedDualForVehicle, FortniteGame)
    DefineUFunction(OnRep_MountedWeaponInfoRepped)

public:
    DefineUStructProperty(FMountedWeaponInfoRepped, MountedWeaponInfoRepped)
    static bool HasMountedWeaponInfoRepped() { return Member_MountedWeaponInfoRepped.IsAvailable(); }
};

// A vehicle's seat weapons (9.00+; the Pirate Cannon's FortVehicleAimingWeaponComp from 9.10): WeaponSeatDefinitions
// names each seat's weapon. The vehicle's seat code tells it when a pawn gets in or out (OnPawnEnterSeat,
// OnPawnExitSeat).
class UFortVehicleSeatWeaponComponent : public UActorComponent
{
    DefineUnrealClass(UFortVehicleSeatWeaponComponent, FortniteGame)
    DefineUFunction(EquipVehicleWeapon)

public:
    DefineUStructArrayProperty(FWeaponSeatDefinition, WeaponSeatDefinitions)
    DefineUProperty(int32_t, ActiveSeatIdx)

    // 21.40's signature; its bool is unnamed in the PDB, and 9.10's body (CL-6573057 0x1155000) doesn't read it.
    void OnPawnEnterSeat(const IFortVehicleInterface* Vehicle, int32_t SeatIdx, bool bFlag);
    // Gives the pawn the seat's weapon as a temporary item and equips it (UnEquipVehicleWeapon, intact, takes it back).
    void EquipVehicleWeapon(AFortPawn* FortPawn, FWeaponSeatDefinition& WeaponSeatDefinition, int32_t ItemLevel);
    // The build's own (9.10 CL-6573057 slot 0x7B, 0x1155090): UnEquipVehicleWeapon for each of the seat's definitions.
    void OnPawnExitSeat(const IFortVehicleInterface* Vehicle, AFortPawn* Pawn, int32_t SeatIdx);

    static void HookVTables();
    // OnPawnEnterSeat's and OnPawnExitSeat's slots once HookVTables restored EquipVehicleWeapon, else -1.
    static int32_t OnPawnEnterSeatSlot() { return OnPawnEnterSeatIndex; }
    static int32_t OnPawnExitSeatSlot() { return OnPawnEnterSeatIndex >= 0 ? OnPawnEnterSeatIndex + 1 : -1; }

private:
    using FOnPawnEnterSeatFunction = void (*)(UFortVehicleSeatWeaponComponent* This, const IFortVehicleInterface* Vehicle, int32_t SeatIdx, bool bFlag);
    // Each class's own: UFortVehicleAimingWeaponComp overrides it (9.10 0x1154ED0, the base's body inlined, then its aiming).
    static inline TMap<void**, FOnPawnEnterSeatFunction> OnPawnEnterSeatOGs;
    // 21.40 has OnPawnExitSeat right after OnPawnEnterSeat (slots 146, 147), as 9.10 and 9.20 do (0x7A, 0x7B).
    static inline int32_t OnPawnEnterSeatIndex = -1;
    static void OnPawnEnterSeatHK(UFortVehicleSeatWeaponComponent* This, const IFortVehicleInterface* Vehicle, int32_t SeatIdx, bool bFlag);
};
