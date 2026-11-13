#include "pch.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include <algorithm>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortLoot, Log, All);

UFortItem* UFortItemDefinition::CreateTemporaryItemInstanceBP(int32_t Count, int32_t Level) const
{
    Reflection::FParams Params(StaticFunction_CreateTemporaryItemInstanceBP());
    Params.Set(L"Count", Count);
    Params.Set(L"Level", Level);
    return Params.Call(const_cast<UFortItemDefinition*>(this)) ? Params.Take<UFortItem*>(L"ReturnValue") : nullptr;
}

FString UFortItemDefinition::GetPersistentName() const
{
    Reflection::FParams Params(StaticFunction_GetPersistentName(), true);
    return Params.Call(const_cast<UFortItemDefinition*>(this)) ? Params.Take<FString>(L"ReturnValue") : FString();
}

uint8_t UFortItemDefinition::GetRarity() const
{
    if (!bCalculateRarityFromQualityAndTier)
    {
        return Rarity;
    }
    int32_t CalculatedRarity = Quality;
    const int32_t ItemTier = Tier;
    if (ItemTier)
    {
        CalculatedRarity += (ItemTier - 1) / 2;
    }
    return static_cast<uint8_t>(CalculatedRarity <= 9 ? CalculatedRarity : 9);
}

void FFortItemEntry::Construct(FFortItemEntry* This, const UFortItemDefinition* InItemDefinition, int32_t InCount, int32_t InLevel)
{
    if (!Natives::FFortItemEntry_FFortItemEntry && !Finder::ResolvePickupLive())
    {
        StaticStruct()->InitializeStruct(This);
        return;
    }
    Natives::FFortItemEntry_FFortItemEntry(This, InItemDefinition, InCount, InLevel, nullptr);
}

ptrdiff_t FFortItemEntry::DurabilityOffset()
{
    return Member_Durability.GetOffsetChecked();
}

int32_t FFortItemEntry::GetIntValue(EFortItemEntryState StateType) const
{
    const TStructArray<FFortItemEntryStateValue>& Values = StateValues;
    for (int32_t Index = 0; Index < Values.Num(); ++Index)
    {
        if (Values[Index].StateType == StateType)
        {
            return Values[Index].IntValue;
        }
    }
    return 0;
}

void FFortItemEntry::SetStateValue(EFortItemEntryState StateType, int32_t IntValue)
{
    if (StateType == Reflection::InvalidEnumValue || (!Natives::FFortItemEntry_SetStateValue && !Finder::ResolveItemEntryStateLive()))
    {
        return;
    }
    Natives::FFortItemEntry_SetStateValue(this, StateType, IntValue);
}

bool FFortItemEntry::RemoveStateValue(EFortItemEntryState StateType)
{
    if (!Natives::FFortItemEntry_MarkDirty && !Finder::ResolveItemEntryStateLive())
    {
        return false;
    }

    TStructArray<FFortItemEntryStateValue>& Values = StateValues;
    for (int32_t Index = 0; Index < Values.Num(); ++Index)
    {
        if (Values[Index].StateType == StateType)
        {
            Values.RemoveAt(Index);
            Natives::FFortItemEntry_MarkDirty(this);
            return true;
        }
    }
    return false;
}

bool FFortItemEntry::ShouldShowItemToast() const
{
    return GetIntValue(EFortItemEntryState::GetShouldShowItemToast()) != 0;
}

void FFortItemEntry::SetNewItemCount(int32_t NewItemCount, bool bShouldShowItemToast)
{
#if WITH_SERVER_CODE
    SetStateValue(EFortItemEntryState::GetNewItemCount(), NewItemCount);
    if (bShouldShowItemToast)
    {
        SetStateValue(EFortItemEntryState::GetShouldShowItemToast(), 1);
    }
#endif
}

void FFortItemEntry::AddNewItemCount(int32_t Count, bool bShouldShowItemToast)
{
    SetNewItemCount(GetIntValue(EFortItemEntryState::GetNewItemCount()) + Count, bShouldShowItemToast);
}

bool FFortItemEntry::LoadFullClip()
{
    // Every entry starts with an empty magazine (the 1.7.2 constructor, 0x65C010, zeroes LoadedAmmo) and a weapon loads
    // its magazine only from its item's LoadedAmmo (0x90B510, through the item's getter 0x66F050). Nothing native fills
    // it for loot any more, so a Battle Royale weapon came out of a chest empty (#85). Weapons that are their own ammo
    // (Athena_Grenade) fire from their stack instead.
    //
    // 6.21's Balloons (Athena_Balloons) are a ranged weapon definition with ItemType Consumable, and their charges are a
    // magazine too: GA_Athena_Balloons costs 1 from AmmoMagazine, its AmmoData is a separate, invisible
    // UFortAmmoItemDefinition (AthenaAmmoDataBalloons) and it has no reload ability, so the clip of its
    // WeaponStatHandle row is all it gets. The weapon takes that from the item on the server (6.21 0x13FC350: Role ==
    // ROLE_Authority, AmmoCount +0x9F0 = the world item's GetLoadedAmmo, slot 0x338), with no item-type check, so a
    // looted stack with LoadedAmmo 0 had no balloons (#431). A consumable is loaded only when its AmmoData is such a
    // separate ammo item; consumables that are their own ammo (bandages, shields) and every other type are unchanged.
    UFortWeaponItemDefinition* WeaponDefinition = Cast<UFortWeaponItemDefinition>(static_cast<UFortItemDefinition*>(ItemDefinition));
    if (!WeaponDefinition)
    {
        return false;
    }
    const EFortItemType ItemType = static_cast<EFortItemType>(WeaponDefinition->ItemType);
    const EFortItemType Consumable = EFortItemType::GetConsumable();
    const bool bRanged = ItemType == EFortItemType::GetWeaponRanged();
    if (!bRanged && (Consumable == Reflection::InvalidEnumValue || ItemType != Consumable))
    {
        return false;
    }
    const TAssetPtr<UFortWorldItemDefinition>& Ammo = WeaponDefinition->AmmoData;
    UFortWorldItemDefinition* AmmoDefinition = Ammo.LoadSynchronous();
    if (!bRanged && !Cast<UFortAmmoItemDefinition>(AmmoDefinition))
    {
        return false;
    }
    const int32_t ClipSize = AmmoDefinition != WeaponDefinition ? WeaponDefinition->GetClipSize() : 0;
    if (ClipSize <= 0 || LoadedAmmo == ClipSize)
    {
        return false;
    }
    LoadedAmmo = ClipSize;
    bIsDirty = true;
    if (!bRanged)
    {
        static std::vector<const UFortWeaponItemDefinition*> Logged;
        if (std::find(Logged.begin(), Logged.end(), WeaponDefinition) == Logged.end())
        {
            Logged.push_back(WeaponDefinition);
            UE_LOG(LogFortLoot, Log, TEXT("%s (consumable) loads its magazine: %d of %s"), *WeaponDefinition->GetName(), ClipSize, *AmmoDefinition->GetName());
        }
    }
    return true;
}

namespace
{
    // Whether a reflected TArray member of FFortItemEntry has elements of property class Kind.
    bool IsArrayOf(const wchar_t* Name, EClassCastFlags Kind)
    {
        UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
        const UProperty* Property = EntryStruct ? EntryStruct->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        const UProperty* Inner = Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UArrayProperty) ? static_cast<const UProperty*>(static_cast<const UArrayProperty*>(Property)->Inner) : nullptr;
        return Inner && Inner->GetClass()->HasAnyCastFlag(Kind);
    }
}

bool FFortItemEntry::HasAlterations()
{
    static const bool bHasAlterations = IsArrayOf(L"AlterationDefinitions", CASTCLASS_UStrProperty) && IsArrayOf(L"AlterationInstances", CASTCLASS_UObjectPropertyBase);
    return bHasAlterations;
}

bool FFortItemEntry::SetAlterations(const TArray<FString>& TemplateIds, const TArray<UObject*>& Definitions)
{
    if (!HasAlterations())
    {
        return false;
    }
    AlterationDefinitions = TemplateIds;
    AlterationInstances = Definitions;
    bIsDirty = true;
    return true;
}

void FFortItemEntry::CopyAlterationsFrom(const FFortItemEntry& Source)
{
    if (HasAlterations() && this != &Source)
    {
        SetAlterations(Source.AlterationDefinitions, Source.AlterationInstances);
    }
}

TArray<UObject*> UFortItem::GetActiveAlterations(int32_t PretendLevel) const
{
    UFunction* Function = FindStaticFunction_GetActiveAlterations();
    if (!Function)
    {
        return {};
    }
    Reflection::FParams Params(Function, true);
    Params.Set(L"PretendLevel", PretendLevel);
    return Params.Call(const_cast<UFortItem*>(this)) ? Params.Take<TArray<UObject*>>(L"ReturnValue") : TArray<UObject*>();
}

int32_t UFortWeaponItemDefinition::GetClipSize() const
{
    const FDataTableRowHandle& StatHandle = WeaponStatHandle;
    UDataTable* StatTable = StatHandle.DataTable;
    const FFortBaseWeaponStats* Stats = StatTable ? StatTable->FindRow<FFortBaseWeaponStats>(StatHandle.RowName, L"UFortWeaponItemDefinition::GetClipSize", false) : nullptr;
    return Stats ? static_cast<int32_t>(Stats->ClipSize) : 0;
}

bool UFortWorldItemDefinition::ServerExecute(const UFortItem* Item, AFortPlayerController* Controller)
{
    if (Natives::UFortWorldItemDefinition_ServerExecuteIndex < 0 && !Finder::ResolveWorldItemDefinitionLive())
    {
        return false;
    }

    using FServerExecute = bool (*)(UFortWorldItemDefinition* This, const UFortItem* Item, AFortPlayerController* Controller);
    return reinterpret_cast<FServerExecute>((*reinterpret_cast<void* const* const*>(this))[Natives::UFortWorldItemDefinition_ServerExecuteIndex])(this, Item, Controller);
}

ptrdiff_t UFortAmmoItemDefinition::RegenCooldownOffset()
{
    return Member_RegenCooldown.GetOffsetChecked();
}

UFortItemDefinition* UFortItem::GetItemDefinitionBP() const
{
    Reflection::FParams Params(StaticFunction_GetItemDefinitionBP());
    return Params.Call(const_cast<UFortItem*>(this)) ? Params.Take<UFortItemDefinition*>(L"ReturnValue") : nullptr;
}

int32_t UFortItem::GetLevel() const
{
    Reflection::FParams Params(StaticFunction_GetLevel());
    return Params.Call(const_cast<UFortItem*>(this)) ? Params.Take<int32_t>(L"ReturnValue") : 0;
}

EFortInventoryFilter UFortItem::GetFilterCategory() const
{
    Reflection::FParams Params(StaticFunction_GetFilterCategory());
    return Params.Call(const_cast<UFortItem*>(this)) ? Params.Take<EFortInventoryFilter>(L"ReturnValue") : EFortInventoryFilter{};
}

bool UFortItem::HasDurability() const
{
    Reflection::FParams Params(StaticFunction_HasDurability());
    return Params.Call(const_cast<UFortItem*>(this)) && Params.Take<bool>(L"ReturnValue");
}

float UFortItem::GetNormalizedDurability() const
{
    Reflection::FParams Params(StaticFunction_GetNormalizedDurability());
    return Params.Call(const_cast<UFortItem*>(this)) ? Params.Take<float>(L"ReturnValue") : 1.0f;
}

float UFortWorldItemDefinition::GetMaxDurability(int32_t ItemLevel) const
{
    Reflection::FParams Params(StaticFunction_GetMaxDurability());
    Params.Set(L"ItemLevel", ItemLevel);
    return Params.Call(const_cast<UFortWorldItemDefinition*>(this)) ? Params.Take<float>(L"ReturnValue") : 1.0f;
}

void UFortWorldItemDefinition::GetDisassemblyResults(TStructArray<FFortItemEntry>& OutResults, float NormalizedDurability) const
{
    if (Natives::UFortWorldItemDefinition_GetDisassemblyResults)
    {
        Natives::UFortWorldItemDefinition_GetDisassemblyResults(this, &OutResults, 0, NormalizedDurability);
    }
}

void UFortNeverPersistItemDefinition::OnItemCountAdded(IFortInventoryOwnerInterface* InventoryOwner, int32_t Count, bool bUnused)
{
    OnItemCountAddedOG(this, InventoryOwner, Count, bUnused);
    AccumulateOnPlayerState(InventoryOwner, Count);
}

void UFortNeverPersistItemDefinition::OnItemCountRemoved(IFortInventoryOwnerInterface* InventoryOwner, int32_t Count)
{
    OnItemCountRemovedOG(this, InventoryOwner, Count);
    AccumulateOnPlayerState(InventoryOwner, -Count);
}

void UFortNeverPersistItemDefinition::AccumulateOnPlayerState(IFortInventoryOwnerInterface* InventoryOwner, int32_t Delta)
{
#if WITH_SERVER_CODE
    if (!bAccumulateOnPlayerState || !InventoryOwner)
    {
        return;
    }

    AFortPlayerController* PlayerController = Cast<AFortPlayerController>(InventoryOwner->_getUObject());
    AFortPlayerStateZone* PlayerState = PlayerController && PlayerController->Role == ROLE_Authority ? Cast<AFortPlayerStateZone>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerController->PlayerState))) : nullptr;
    if (PlayerState)
    {
        PlayerState->AddAccumulatedItem(this, Delta);
    }
#endif
}

void UFortNeverPersistItemDefinition::HookVTables()
{
    UObject* NeverPersistDefault = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** NeverPersistVTable = NeverPersistDefault ? *reinterpret_cast<void***>(NeverPersistDefault) : nullptr;
    if (!NeverPersistVTable || !Finder::ResolveNeverPersistItemDefinitionLive())
    {
        return;
    }

    const int32_t AddedIndex = Natives::UFortNeverPersistItemDefinition_OnItemCountAddedIndex;
    const int32_t RemovedIndex = Natives::UFortNeverPersistItemDefinition_OnItemCountRemovedIndex;
    void* const Added = NeverPersistVTable[AddedIndex];
    void* const Removed = NeverPersistVTable[RemovedIndex];
    OnItemCountAddedOG = reinterpret_cast<decltype(OnItemCountAddedOG)>(Added);
    OnItemCountRemovedOG = reinterpret_cast<decltype(OnItemCountRemovedOG)>(Removed);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[AddedIndex] == Added && VTable[RemovedIndex] == Removed)
        {
            Hooks::SwapVTable(L"UFortNeverPersistItemDefinition::OnItemCountAdded", VTable, AddedIndex, reinterpret_cast<void*>(&OnItemCountAddedHK));
            Hooks::SwapVTable(L"UFortNeverPersistItemDefinition::OnItemCountRemoved", VTable, RemovedIndex, reinterpret_cast<void*>(&OnItemCountRemovedHK));
        }
    }
}

void UFortNeverPersistItemDefinition::OnItemCountAddedHK(UFortNeverPersistItemDefinition* This, IFortInventoryOwnerInterface* InventoryOwner, int32_t Count, bool bUnused)
{
    This->OnItemCountAdded(InventoryOwner, Count, bUnused);
}

void UFortNeverPersistItemDefinition::OnItemCountRemovedHK(UFortNeverPersistItemDefinition* This, IFortInventoryOwnerInterface* InventoryOwner, int32_t Count)
{
    This->OnItemCountRemoved(InventoryOwner, Count);
}

void UFortWorldItem::SetOwningControllerForTemporaryItem(AFortPlayerController* InController)
{
    Reflection::FParams Params(StaticFunction_SetOwningControllerForTemporaryItem());
    Params.Set(L"InController", InController);
    Params.Call(this);
}

bool UFortWorldItem::SetItemCount(int32_t NewCount)
{
    if (Natives::UFortWorldItem_SetItemCountIndex < 0 && !Finder::ResolveWorldItemCountLive(UFortAmmoItemDefinition::RegenCooldownOffset(), ItemEntryOffset()))
    {
        return false;
    }

    using FSetItemCount = bool (*)(UFortWorldItem* This, int32_t NewCount);
    return reinterpret_cast<FSetItemCount>((*reinterpret_cast<void* const* const*>(this))[Natives::UFortWorldItem_SetItemCountIndex])(this, NewCount);
}

ptrdiff_t UFortWorldItem::ItemEntryOffset()
{
    return Member_ItemEntry.GetOffsetChecked();
}

bool UFortWorldItem::SetLoadedAmmo(int32_t NewLoadedAmmo)
{
#if WITH_SERVER_CODE
    FFortItemEntry& Entry = ItemEntry;
    if (Entry.LoadedAmmo == NewLoadedAmmo)
    {
        return false;
    }

    const FWeakObjectPtr& ParentInventory = Entry.ParentInventory;
    if (AFortInventory* Inventory = Cast<AFortInventory>(ParentInventory.Get()))
    {
        Inventory->UpdateWorldItemLoadedAmmo(this, NewLoadedAmmo);
    }
    else
    {
        Entry.LoadedAmmo = NewLoadedAmmo;
    }
    return true;
#else
    return false;
#endif
}

bool UFortWorldItem::SetDurability(float NewDurability)
{
#if WITH_SERVER_CODE
    if (!HasDurability())
    {
        return false;
    }

    FFortItemEntry& Entry = ItemEntry;
    if (Entry.Durability != NewDurability)
    {
        Entry.Durability = NewDurability;
        if (OwnerInventory)
        {
            bNeedsPersistentUpdate = true;
        }
        if (Natives::FFortItemEntry_MarkDirty || Finder::ResolveItemEntryStateLive())
        {
            Natives::FFortItemEntry_MarkDirty(&Entry);
        }
    }

    const EFortItemEntryState DurabilityInitialized = EFortItemEntryState::GetDurabilityInitialized();
    if (Entry.GetIntValue(DurabilityInitialized) == 0)
    {
        Entry.SetStateValue(DurabilityInitialized, 1);
    }
    return true;
#else
    return false;
#endif
}

void UFortWorldItem::HookVTables()
{
    UObject* WorldItemDefault = StaticClass()->GetDefaultObject(false);
    void** WorldItemVTable = WorldItemDefault ? *reinterpret_cast<void***>(WorldItemDefault) : nullptr;
    UObject* PlayerControllerDefault = AFortPlayerController::StaticClass()->GetDefaultObject(false);
    const ptrdiff_t InterfaceOffset = Natives::AFortPlayerController_InventoryOwnerInterface;
    void** InventoryOwnerVTable = PlayerControllerDefault && InterfaceOffset > 0 ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(PlayerControllerDefault) + InterfaceOffset) : nullptr;
    if (!WorldItemVTable || !Finder::ResolveWorldItemLive(InventoryOwnerVTable))
    {
        return;
    }

    const int32_t Index = Natives::UFortWorldItem_SetLoadedAmmoIndex;
    void* const Stripped = WorldItemVTable[Index];
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Stripped)
        {
            Hooks::SwapVTable(L"UFortWorldItem::SetLoadedAmmo", VTable, Index, reinterpret_cast<void*>(&SetLoadedAmmoHK));
        }
    }

    if (!AFortPlayerController::ResolveDurability())
    {
        return;
    }

    const int32_t DurabilityIndex = Natives::UFortWorldItem_SetDurabilityIndex;
    void* const StrippedDurability = WorldItemVTable[DurabilityIndex];
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[DurabilityIndex] == StrippedDurability)
        {
            Hooks::SwapVTable(L"UFortWorldItem::SetDurability", VTable, DurabilityIndex, reinterpret_cast<void*>(&SetDurabilityHK));
        }
    }
}

bool UFortWorldItem::SetLoadedAmmoHK(UFortWorldItem* This, int32_t NewLoadedAmmo)
{
    return This->SetLoadedAmmo(NewLoadedAmmo);
}

bool UFortWorldItem::SetDurabilityHK(UFortWorldItem* This, float NewDurability)
{
    return This->SetDurability(NewDurability);
}

