#include "pch.h"
#include "Abilities/FortGameplayAbility.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "Weapons/FortWeapon.h"

#include <format>
#include <string>

AFortPawn* UFortGameplayAbility::GetActivatingPawn() const
{
    Reflection::FParams Params(StaticFunction_GetActivatingPawn());
    return Params.Call(const_cast<UFortGameplayAbility*>(this)) ? Params.Take<AFortPawn*>(L"ReturnValue") : nullptr;
}

bool UFortGameplayAbility::BP_GetGameplayEffectContainer(const FGameplayTag& ApplicationTag, void* OutContainer) const
{
    UFunction* Function = FindStaticFunction_BP_GetGameplayEffectContainer();
    UScriptStruct* ContainerStruct = FFortGameplayEffectContainer::StaticStruct();
    if (!Function || !ContainerStruct || !OutContainer)
    {
        return false;
    }
    Reflection::FParams Params(Function, true);
    if (!Params.Set(L"ApplicationTag", ApplicationTag) || !Params.Call(const_cast<UFortGameplayAbility*>(this)))
    {
        return false;
    }
    const void* Found = Params.GetAddress(L"ReturnValue");
    if (!Found)
    {
        return false;
    }
    ContainerStruct->CopyScriptStruct(OutContainer, Found);
    return true;
}

int32_t UFortGameplayAbility::IncludeEnemyTargets()
{
    // EffectContainers is a fixed array: its property says how many there are and how far apart.
    const UProperty* Property = StaticClass()->FindPropertyByName(FName(L"EffectContainers", FNAME_Find));
    if (!Property)
    {
        return 0;
    }

    int32_t Changed = 0;
    for (int32_t ContainerIndex = 0; ContainerIndex < Property->ArrayDim; ++ContainerIndex)
    {
        FFortGameplayEffectContainer& Container = *Property->ContainerPtrToValuePtr<FFortGameplayEffectContainer>(this, ContainerIndex);
        TStructArray<FFortAbilityTargetSelection>& Selections = Container.TargetSelection.List;
        for (int32_t SelectionIndex = 0; SelectionIndex < Selections.Num(); ++SelectionIndex)
        {
            FFortTargetFilter& Filter = Selections[SelectionIndex].TargetFilter;
            if (Filter.bExcludePawnEnemies)
            {
                Filter.bExcludePawnEnemies = false;
                ++Changed;
            }
        }
    }
    return Changed;
}

TWeakObjectPtr<UFortWorldItem>& UFortGameplayAbility::GetAbilityGrantedWeaponItem() const
{
    const ptrdiff_t Offset = Member_SmallPreviewImageOverride.GetOffsetChecked() + sizeof(UTexture2D*);
    return *reinterpret_cast<TWeakObjectPtr<UFortWorldItem>*>(reinterpret_cast<uintptr_t>(this) + Offset);
}

TWeakObjectPtr<AFortWeapon>* UFortGameplayAbility::GetAbilityGrantedWeapon() const
{
    const ptrdiff_t Offset = Natives::UFortGameplayAbility_AbilityGrantedWeapon;
    return Offset > 0 ? reinterpret_cast<TWeakObjectPtr<AFortWeapon>*>(reinterpret_cast<uintptr_t>(this) + Offset) : nullptr;
}

void UFortGameplayAbility::EquipAbilityWeapon(UFortWeaponItemDefinition* WeaponDefinition, int32_t ItemLevel)
{
#if WITH_SERVER_CODE
    AFortPawn* Pawn = GetActivatingPawn();
    if (!Pawn || Pawn->Role != ROLE_Authority || !WeaponDefinition)
    {
        return;
    }

    AFortPlayerController* PlayerController = Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Pawn->Controller)));
    AFortInventory* Inventory = PlayerController ? static_cast<AFortInventory*>(PlayerController->WorldInventory) : nullptr;
    if (!Inventory)
    {
        return;
    }

    UFortWorldItem* WorldItem = Cast<UFortWorldItem>(WeaponDefinition->CreateTemporaryItemInstanceBP(1, ItemLevel));
    if (!WorldItem)
    {
        return;
    }

    WorldItem->SetOwningControllerForTemporaryItem(PlayerController);

    // A player in a mounted turret couldn't fire it (#461). The weapon gets its magazine from its item: on the server,
    // AFortWeapon sets AmmoCount (+0x794) to the world item's GetLoadedAmmo (6.31 0x1457650, the item's slot 0x67). A
    // new item's magazine is empty (#85), so this item came with none. MountedTurret_Weapon is meant to stay loaded:
    // ClipSize 1,000,000, InitialClips 1, no ReloadAbility, and AmmoInfinite as its AmmoData, which no player carries.
    // So it could neither fire nor reload. The item is loaded before it goes into the inventory, so the replicated
    // entry has the rounds too. Melee and other non-ranged ability weapons are left as they are.
    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    ItemEntry.LoadFullClip();
    Inventory->AddWorldItem(WorldItem);
    GetAbilityGrantedWeaponItem() = TWeakObjectPtr<UFortWorldItem>(WorldItem);

    const FGuid ItemGuid = ItemEntry.ItemGuid;
    AFortWeapon* Weapon = Pawn->EquipWeaponDefinition(WeaponDefinition, ItemGuid);
    // The weapon too (AbilityGrantedWeapon), which the build's EndAbility cleanup gives its lifespan when the ability ends.
    if (TWeakObjectPtr<AFortWeapon>* GrantedWeapon = Weapon ? GetAbilityGrantedWeapon() : nullptr)
    {
        *GrantedWeapon = TWeakObjectPtr<AFortWeapon>(Weapon);
    }
    Log::Info(L"Ability weapon: {} gives {} {} ({} rounds loaded){}", GetName().ToStdString(), Pawn->GetName().ToStdString(), WeaponDefinition->GetName().ToStdString(),
        static_cast<int32_t>(ItemEntry.LoadedAmmo), Weapon ? L"" : L", but the pawn equipped nothing");
#endif
}

// The X-4 Stormwing's guns stayed in the player's view after they got out (#480: the camera stayed the plane gun's,
// B_Ranged_Dual_Generic_FerretVehicle's CameraBase3PClass Athena_PlayerCameraModeMountedTurret). GA_FerretVehicleWeapon
// gives them with EquipAbilityWeapon and its EndAbility takes them back, as the mounted turret's ability does (#461).
// The game's own EndAbility cleanup (7.00 0x9ECFE0, 7.10 0x9FE9D0, run right after this) shows Epic's order: the held
// weapon goes through the pawn's RemoveAbilityWeapon (7.00 slot 0x1CF: out of CurrentWeaponList, UnEquip, CurrentWeapon
// cleared), a server-only step that is stripped (a return-zero stub, 7.00 0x2A0730 / 7.10 0x2A7140, called with the
// player controller), the item leaves the inventory (the controller's RemoveInventoryItem, bForceRemoval), and then the
// weapon in AbilityGrantedWeapon gets SetLifeSpan(3.0) (vtable +0x358): it isn't destroyed there and then. Core used to
// destroy it at once, before the client had swapped to the weapon put back in its hands, and never set
// AbilityGrantedWeapon. Where the cleanup reads AbilityGrantedWeapon (Natives::UFortGameplayAbility_AbilityGrantedWeapon,
// HookNatives), the weapon is now left to that lifespan; elsewhere it is destroyed as before.
void UFortGameplayAbility::RemoveAbilityWeapon()
{
#if WITH_SERVER_CODE
    UFortWorldItem* WorldItem = GetAbilityGrantedWeaponItem().Get();
    AFortPawn* Pawn = WorldItem ? GetActivatingPawn() : nullptr;
    if (!Pawn || Pawn->Role != ROLE_Authority)
    {
        if (WorldItem)
        {
            Log::Warning(L"Ability weapon: {} ends with {} granted but no pawn with authority to take it from", GetName().ToStdString(), WorldItem->GetName().ToStdString());
        }
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const FGuid ItemGuid = ItemEntry.ItemGuid;
    const bool bLifespanByCleanup = GetAbilityGrantedWeapon() != nullptr;
    TArray<AFortWeapon*>& Weapons = Pawn->CurrentWeaponList;
    for (int32_t Index = 0; Index < Weapons.Num(); ++Index)
    {
        AFortWeapon* Weapon = Weapons[Index];
        if (Weapon && Weapon->ItemEntryGuid == ItemGuid)
        {
            const bool bHeld = static_cast<AFortWeapon*>(Pawn->CurrentWeapon) == Weapon;
            Pawn->RemoveAbilityWeapon(Weapon, !bLifespanByCleanup);
            AFortWeapon* Held = static_cast<AFortWeapon*>(Pawn->CurrentWeapon);
            Log::Info(L"Ability weapon: {} takes {} from {} ({}; {})", GetName().ToStdString(), Weapon->GetName().ToStdString(), Pawn->GetName().ToStdString(),
                bHeld ? std::format(L"it was in hand, {} is now", Held ? Held->GetName().ToStdString() : std::wstring(L"nothing")) : std::wstring(L"it wasn't in hand"),
                bLifespanByCleanup ? L"EndAbility gives it its lifespan" : L"destroyed");
            return;
        }
    }
    Log::Warning(L"Ability weapon: {} ends but {} has no weapon of its item {} ({} in its weapon list)", GetName().ToStdString(), Pawn->GetName().ToStdString(),
        WorldItem->GetName().ToStdString(), Weapons.Num());
#endif
}

DEFINE_FUNCTION(UFortGameplayAbility::execEquipAbilityWeapon)
{
    P_GET_OBJECT(UFortWeaponItemDefinition, Z_Param_WeaponDefinition);
    P_GET_PROPERTY(UIntProperty, Z_Param_ItemLevel);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortGameplayAbility*>(Context)->EquipAbilityWeapon(Z_Param_WeaponDefinition, Z_Param_ItemLevel);
    P_NATIVE_END;
}

void UFortGameplayAbility::HookNatives()
{
    if (UFunction* Function = StaticFunction_EquipAbilityWeapon(); Function && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        Function->SetNativeFunc(&execEquipAbilityWeapon);
        Log::Info(L"Hook UFortGameplayAbility::EquipAbilityWeapon: native");
    }

    const ptrdiff_t AbilityGrantedWeaponItemOffset = Member_SmallPreviewImageOverride.GetOffsetChecked() + sizeof(UTexture2D*);
    if (uint8_t* Call = Finder::AbilityEndCleanupCall(AbilityGrantedWeaponItemOffset))
    {
        EndAbilityCleanupOG = reinterpret_cast<decltype(EndAbilityCleanupOG)>(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(Call), 1, 5));
        Hooks::RedirectCall(L"UFortGameplayAbility::EndAbility -> RemoveAbilityWeapon", Call, reinterpret_cast<void*>(&EndAbilityCleanupCall));

        // AbilityGrantedWeapon follows AbilityGrantedWeaponItem (21.40's layout). A build whose cleanup hands both weak
        // pointers to their helpers (lea rcx, [rdi+8E8h] and lea rsi, [rdi+8F0h] on 7.00 and 7.10) gives the weapon its
        // lifespan (RemoveAbilityWeapon above); one whose cleanup doesn't keeps Core's destroy.
        const ptrdiff_t AbilityGrantedWeaponOffset = AbilityGrantedWeaponItemOffset + static_cast<ptrdiff_t>(sizeof(FWeakObjectPtr));
        const uintptr_t Cleanup = reinterpret_cast<uintptr_t>(EndAbilityCleanupOG);
        if (Finder::TakesMemberAddressAt(Cleanup, static_cast<int32_t>(AbilityGrantedWeaponItemOffset))
            && Finder::TakesMemberAddressAt(Cleanup, static_cast<int32_t>(AbilityGrantedWeaponOffset)))
        {
            Natives::UFortGameplayAbility_AbilityGrantedWeapon = AbilityGrantedWeaponOffset;
            Log::Info(L"Finder: UFortGameplayAbility::AbilityGrantedWeapon +0x{:X}, given its lifespan by the EndAbility cleanup", AbilityGrantedWeaponOffset);
        }
        else
        {
            Log::Info(L"Finder: the EndAbility cleanup reads no AbilityGrantedWeapon (+0x{:X}); ability weapons are destroyed when taken", AbilityGrantedWeaponOffset);
        }
    }
}

void UFortGameplayAbility::EndAbilityCleanupCall(UFortGameplayAbility* This)
{
    This->RemoveAbilityWeapon();
    EndAbilityCleanupOG(This);
}
