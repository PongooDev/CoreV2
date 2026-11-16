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
    Log::Info(L"Ability weapon: {} gives {} {} ({} rounds loaded){}", GetName().ToStdString(), Pawn->GetName().ToStdString(), WeaponDefinition->GetName().ToStdString(),
        static_cast<int32_t>(ItemEntry.LoadedAmmo), Weapon ? L"" : L", but the pawn equipped nothing");
#endif
}

void UFortGameplayAbility::RemoveAbilityWeapon()
{
#if WITH_SERVER_CODE
    UFortWorldItem* WorldItem = GetAbilityGrantedWeaponItem().Get();
    AFortPawn* Pawn = WorldItem ? GetActivatingPawn() : nullptr;
    if (!Pawn || Pawn->Role != ROLE_Authority)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const FGuid ItemGuid = ItemEntry.ItemGuid;
    TArray<AFortWeapon*>& Weapons = Pawn->CurrentWeaponList;
    for (int32_t Index = 0; Index < Weapons.Num(); ++Index)
    {
        AFortWeapon* Weapon = Weapons[Index];
        if (Weapon && Weapon->ItemEntryGuid == ItemGuid)
        {
            const bool bHeld = static_cast<AFortWeapon*>(Pawn->CurrentWeapon) == Weapon;
            Pawn->RemoveAbilityWeapon(Weapon);
            Log::Info(L"Ability weapon: {} takes {} from {} ({})", GetName().ToStdString(), Weapon->GetName().ToStdString(), Pawn->GetName().ToStdString(),
                bHeld ? L"it was in hand, the pawn's previous weapon is equipped" : L"it wasn't in hand");
            return;
        }
    }
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
    }
}

void UFortGameplayAbility::EndAbilityCleanupCall(UFortGameplayAbility* This)
{
    This->RemoveAbilityWeapon();
    EndAbilityCleanupOG(This);
}
