#include "pch.h"
#include "Vehicles/FortVehicleSeatWeaponComponent.h"
#include "Athena/FortAthenaVehicle.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

namespace
{
    // AFortAthenaVehicle::GetPawnAtSeat, an intact native.
    UFunction* GetPawnAtSeatFunction = nullptr;

    AFortPawn* GetPawnAtSeat(AActor* Vehicle, int32_t SeatIdx)
    {
        if (!Vehicle || !GetPawnAtSeatFunction)
        {
            return nullptr;
        }
        Reflection::FParams Params(GetPawnAtSeatFunction, true);
        if (!Params.Set<int32_t>(L"SeatIdx", SeatIdx))
        {
            return nullptr;
        }
        Params.Call(Vehicle);
        UObject** Result = Params.Get<UObject*>(L"ReturnValue");
        return Result ? Cast<AFortPawn>(*Result) : nullptr;
    }
}

TWeakObjectPtr<UFortWorldItem>& FWeaponSeatDefinition::GetVehicleGrantedWeaponItem()
{
    uint8_t* const LastEquipped = reinterpret_cast<uint8_t*>(&GetMember_LastEquippedVehicleWeapon());
    return *reinterpret_cast<TWeakObjectPtr<UFortWorldItem>*>(LastEquipped + sizeof(UFortWeaponItemDefinition*));
}

TWeakObjectPtr<AFortWeapon>& FWeaponSeatDefinition::GetVehicleGrantedWeapon()
{
    uint8_t* const LastEquipped = reinterpret_cast<uint8_t*>(&GetMember_LastEquippedVehicleWeapon());
    return *reinterpret_cast<TWeakObjectPtr<AFortWeapon>*>(LastEquipped + sizeof(UFortWeaponItemDefinition*) + sizeof(FWeakObjectPtr));
}

bool FWeaponSeatDefinition::HasGrantedWeapon()
{
    UScriptStruct* const Struct = StaticStruct();
    return Struct && Member_LastEquippedVehicleWeapon.IsAvailable()
        && Struct->GetStructureSize() == Member_LastEquippedVehicleWeapon.GetOffsetChecked() + static_cast<int32_t>(sizeof(UFortWeaponItemDefinition*) + 2 * sizeof(FWeakObjectPtr));
}

// 9.10's body (CL-6573057 0x1155000) sets ActiveSeatIdx for the seat's definitions and tells the vehicle; the server part
// that equipped the seat's weapon is compiled out: EquipVehicleWeapon's exec (0x1CFBD00) calls the shared empty
// function and nothing else in the image calls it. 9.10 moved the Pirate Cannon's weapons here (FortVehicleAimingWeaponComp
// names ShipCannon_Weapon for seat 0 and ShipCannon_Weapon_InCannon for seat 1), and GA_ShipCannon_Enter no longer
// calls EquipAbilityWeapon, so a player in the cannon kept their pickaxe (#693). OnPawnExitSeat (0x1155090) is intact
// and calls UnEquipVehicleWeapon (0x1160F80), which takes the granted item and weapon back, but AFortAthenaVehicle's
// OnExitVehicle no longer calls it (see AFortAthenaVehicle::OnExitVehicle). UFortVehicleAimingWeaponComp's override
// (0x1154ED0) inlines this body and has no equip either; each class runs its own first.
void UFortVehicleSeatWeaponComponent::OnPawnEnterSeat(const IFortVehicleInterface* Vehicle, int32_t SeatIdx, bool bFlag)
{
    if (const FOnPawnEnterSeatFunction* Original = OnPawnEnterSeatOGs.Find(*reinterpret_cast<void***>(this)))
    {
        (*Original)(this, Vehicle, SeatIdx, bFlag);
    }
#if WITH_SERVER_CODE
    AActor* const VehicleActor = GetOwner();
    if (!VehicleActor || VehicleActor->Role != ROLE_Authority)
    {
        return;
    }
    AFortPawn* const Pawn = GetPawnAtSeat(VehicleActor, SeatIdx);
    TStructArray<FWeaponSeatDefinition>& Definitions = GetMember_WeaponSeatDefinitions();
    // A seat change (AFortPlayerControllerZone::ServerRequestSeatChange_Implementation, 9.10 0x1879780 -> 0x12F6A30; 9.20
    // 0x18F6570 -> 0x1355F70 and the interface's slot 103) empties the old seat and seats the pawn in the new one without
    // OnExitVehicle, so nothing takes the old seat's weapon back. Core's: an empty seat whose weapon is still in this
    // pawn's inventory gets the pawn's OnPawnExitSeat first.
    AFortPlayerController* const PlayerController = Pawn ? Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Pawn->Controller))) : nullptr;
    AFortInventory* const Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    for (int32_t Index = 0; Inventory && Index < Definitions.Num(); ++Index)
    {
        FWeaponSeatDefinition& Definition = Definitions[Index];
        UFortWorldItem* const Granted = Definition.SeatIndex != SeatIdx ? Definition.GetVehicleGrantedWeaponItem().Get() : nullptr;
        if (Granted && !GetPawnAtSeat(VehicleActor, Definition.SeatIndex) && Inventory->FindWorldItem(Granted->ItemEntry.ItemGuid) == Granted)
        {
            Log::Info(L"Vehicle weapon: {} moves from seat {} to seat {}; {} takes back what seat {} gave", Pawn->GetName().ToStdString(), Definition.SeatIndex, SeatIdx,
                GetName().ToStdString(), Definition.SeatIndex);
            OnPawnExitSeat(Vehicle, Pawn, Definition.SeatIndex);
        }
    }
    for (int32_t Index = 0; Pawn && Index < Definitions.Num(); ++Index)
    {
        FWeaponSeatDefinition& Definition = Definitions[Index];
        if (Definition.SeatIndex == SeatIdx && Definition.VehicleWeapon)
        {
            // Core's: level 0, the definitions name none.
            EquipVehicleWeapon(Pawn, Definition, 0);
        }
    }
#endif
}

void UFortVehicleSeatWeaponComponent::EquipVehicleWeapon(AFortPawn* FortPawn, FWeaponSeatDefinition& WeaponSeatDefinition, int32_t ItemLevel)
{
#if WITH_SERVER_CODE
    UFortWeaponItemDefinition* const WeaponDefinition = WeaponSeatDefinition.VehicleWeaponOverride ? WeaponSeatDefinition.VehicleWeaponOverride : WeaponSeatDefinition.VehicleWeapon;
    if (!FortPawn || FortPawn->Role != ROLE_Authority || !WeaponDefinition)
    {
        return;
    }
    if (UFortWorldItem* const Granted = WeaponSeatDefinition.GetVehicleGrantedWeaponItem().Get())
    {
        Log::Warning(L"Vehicle weapon: {} gives {} no {} for seat {}: the seat's last one ({}) was never taken back", GetName().ToStdString(),
            FortPawn->GetName().ToStdString(), WeaponDefinition->GetName().ToStdString(), WeaponSeatDefinition.SeatIndex, Granted->GetName().ToStdString());
        return;
    }
    AFortPlayerController* const PlayerController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(FortPawn->Controller)));
    AFortInventory* const Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    UFortWorldItem* const WorldItem = Inventory ? Cast<UFortWorldItem>(WeaponDefinition->CreateTemporaryItemInstanceBP(1, ItemLevel)) : nullptr;
    if (!WorldItem)
    {
        return;
    }

    // As UFortGameplayAbility::EquipAbilityWeapon: the cannon's weapons have no reload and AmmoInfinite, so the item is
    // loaded before it goes into the inventory.
    WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    ItemEntry.LoadFullClip();
    Inventory->AddWorldItem(WorldItem);
    WeaponSeatDefinition.GetVehicleGrantedWeaponItem() = TWeakObjectPtr<UFortWorldItem>(WorldItem);

    const FGuid ItemGuid = ItemEntry.ItemGuid;
    AFortWeapon* const Weapon = FortPawn->EquipWeaponDefinition(WeaponDefinition, ItemGuid);
    WeaponSeatDefinition.GetVehicleGrantedWeapon() = TWeakObjectPtr<AFortWeapon>(Weapon);

    // Core's: the weapon's IFortMountedWeaponInterface::SetupCachedHostVehicleRepped, which its AttachWeaponTo (0x1A12A20)
    // calls while it is equipped, is stripped too (the interface's slot 8 is the shared empty function). It would have
    // set the host vehicle and seat the clients read in OnRep_MountedWeaponInfoRepped. They are set here, and the
    // OnRep's own code is run on the server: it hands the weapon to the vehicle (the seat component's CachedWeapon and
    // bWeaponEquipped) and attaches it to the vehicle.
    AFortWeaponRangedForVehicle* const VehicleWeapon = Cast<AFortWeaponRangedForVehicle>(Weapon);
    UFunction* const OnRep = VehicleWeapon ? AFortWeaponRangedForVehicle::FindStaticFunction_OnRep_MountedWeaponInfoRepped() : nullptr;
    if (OnRep)
    {
        FMountedWeaponInfoRepped& Repped = VehicleWeapon->MountedWeaponInfoRepped;
        Repped.HostVehicleCachedActor = GetOwner();
        Repped.HostVehicleSeatIndexCached = WeaponSeatDefinition.SeatIndex;
        Reflection::FParams Params(OnRep, true);
        Params.Call(VehicleWeapon);
    }
    else if (AFortWeaponRangedDualForVehicle* const DualWeapon = Cast<AFortWeaponRangedDualForVehicle>(Weapon))
    {
        // Mounted turrets use the dual class. CL-6922310's interface slot 8 is a stub (0x3528D0), and the dual
        // muzzle routine (0x1A9BC40) uses the cached vehicle and seat to resolve the vehicle's firing socket.
        UFunction* const DualOnRep = AFortWeaponRangedDualForVehicle::FindStaticFunction_OnRep_MountedWeaponInfoRepped();
        if (DualOnRep && FMountedWeaponInfoRepped::HasHostVehicle() && AFortWeaponRangedDualForVehicle::HasMountedWeaponInfoRepped())
        {
            FMountedWeaponInfoRepped& Repped = DualWeapon->MountedWeaponInfoRepped;
            if (!Repped.HostVehicleCachedActor)
            {
                Repped.HostVehicleCachedActor = GetOwner();
                Repped.HostVehicleSeatIndexCached = WeaponSeatDefinition.SeatIndex;
                Reflection::FParams Params(DualOnRep, true);
                Params.Call(DualWeapon);
            }
        }
    }

    Log::Info(L"Vehicle weapon: {} gives {} {} for seat {}{}", GetName().ToStdString(), FortPawn->GetName().ToStdString(), WeaponDefinition->GetName().ToStdString(),
        WeaponSeatDefinition.SeatIndex, Weapon ? L"" : L", but the pawn equipped nothing");
#endif
}

void UFortVehicleSeatWeaponComponent::OnPawnExitSeat(const IFortVehicleInterface* Vehicle, AFortPawn* Pawn, int32_t SeatIdx)
{
    if (OnPawnExitSeatSlot() >= 0)
    {
        CallVirtual<void>(this, OnPawnExitSeatSlot(), Vehicle, Pawn, SeatIdx);
    }
}

void UFortVehicleSeatWeaponComponent::OnPawnEnterSeatHK(UFortVehicleSeatWeaponComponent* This, const IFortVehicleInterface* Vehicle, int32_t SeatIdx, bool bFlag)
{
    This->OnPawnEnterSeat(Vehicle, SeatIdx, bFlag);
}

void UFortVehicleSeatWeaponComponent::HookVTables()
{
#if WITH_SERVER_CODE
    // Only where EquipVehicleWeapon is reflected (9.10+) and its exec ends in a stub: 9.00 and 9.01 have the component
    // without it, and their cannon gives its weapons from GA_ShipCannon_Enter (EquipAbilityWeapon).
    UFunction* const Equip = StaticClass() ? FindStaticFunction_EquipVehicleWeapon() : nullptr;
    const uintptr_t Exec = Equip ? reinterpret_cast<uintptr_t>(Equip->GetNativeFunc()) : 0;
    bool bExecCallsStub = false;
    for (const auto& [Site, Target] : Exec ? Finder::DecodedDirectCalls(Exec) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        bExecCallsStub = bExecCallsStub || Finder::IsStubFunction(Target);
    }
    if (!bExecCallsStub)
    {
        if (Equip)
        {
            Log::Info(L"Finder: UFortVehicleSeatWeaponComponent::EquipVehicleWeapon is native in this build");
        }
        return;
    }
    UClass* const VehicleClass = AFortAthenaVehicle::StaticClass();
    GetPawnAtSeatFunction = VehicleClass ? VehicleClass->FindFunctionByName(FName(L"GetPawnAtSeat", FNAME_Find)) : nullptr;
    const int32_t Index = FWeaponSeatDefinition::HasGrantedWeapon() && Member_ActiveSeatIdx.IsAvailable()
        ? Finder::VehicleSeatWeaponOnPawnEnterSeatSlot(Member_WeaponSeatDefinitions.GetOffsetChecked(), Member_ActiveSeatIdx.GetOffsetChecked())
        : -1;
    UObject* const Default = StaticClass()->GetDefaultObject(false);
    void** const DefaultVTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
    if (!GetPawnAtSeatFunction || Index < 0 || !DefaultVTable)
    {
        Log::Error(L"Finder: UFortVehicleSeatWeaponComponent::EquipVehicleWeapon is stripped and OnPawnEnterSeat (slot {}) or GetPawnAtSeat ({}) wasn't found, so vehicle seats give no weapon",
            Index, GetPawnAtSeatFunction != nullptr);
        return;
    }

    int32_t Swapped = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UObject* const ClassDefault = It->IsChildOf(StaticClass()) ? It->GetDefaultObject(false) : nullptr;
        void** const VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        void* const ClassEnter = VTable ? VTable[Index] : nullptr;
        if (ClassEnter && ClassEnter != reinterpret_cast<void*>(&OnPawnEnterSeatHK) && !OnPawnEnterSeatOGs.Contains(VTable))
        {
            OnPawnEnterSeatOGs.Add(VTable, reinterpret_cast<FOnPawnEnterSeatFunction>(ClassEnter));
            Hooks::SwapVTable(L"UFortVehicleSeatWeaponComponent::OnPawnEnterSeat", VTable, Index, reinterpret_cast<void*>(&OnPawnEnterSeatHK));
            ++Swapped;
        }
    }
    OnPawnEnterSeatIndex = Index;
    Log::Info(L"Finder: UFortVehicleSeatWeaponComponent::EquipVehicleWeapon is stripped: OnPawnEnterSeat (slot 0x{:X}) equips the seat's weapon after the build's, in {} vtables", Index, Swapped);
#endif
}
