#include "pch.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Finder.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Log.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"
#include "Natives.h"
#include "ReflectionParams.h"

#include <map>
#include <string>

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

uint8_t UFortItemDefinition::GetFilterCategory() const
{
    Reflection::FParams Params(StaticFunction_GetFilterCategory());
    return Params.Call(const_cast<UFortItemDefinition*>(this)) ? Params.Take<uint8_t>(L"ReturnValue") : 0;
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

uint8_t FFortItemEntry::GetStateType(const wchar_t* StateName)
{
    static std::map<std::wstring, uint8_t> StateTypes;
    if (const auto Found = StateTypes.find(StateName); Found != StateTypes.end())
    {
        return Found->second;
    }

    UObject* StateEnum = Natives::StaticFindObject(nullptr, nullptr, L"/Script/FortniteGame.EFortItemEntryState", false);
    UFunction* GetEnumeratorName = FindObject<UFunction>(nullptr, L"/Script/Engine.KismetNodeHelperLibrary:GetEnumeratorName");
    UClass* NodeHelperLibrary = FindObject<UClass>(nullptr, L"/Script/Engine.KismetNodeHelperLibrary");
    UObject* NodeHelperDefault = NodeHelperLibrary ? NodeHelperLibrary->GetDefaultObject() : nullptr;
    const FName Wanted((L"EFortItemEntryState::" + std::wstring(StateName)).c_str(), FNAME_Find);
    uint8_t StateType = 0xFF;
    for (int32_t Value = 0; StateEnum && GetEnumeratorName && NodeHelperDefault && Value < 0xFF && StateType == 0xFF; ++Value)
    {
        Reflection::FParams Params(GetEnumeratorName);
        Params.Set(L"Enum", StateEnum);
        Params.Set(L"EnumeratorValue", static_cast<uint8_t>(Value));
        if (Params.Call(NodeHelperDefault) && Params.Take<FName>(L"ReturnValue") == Wanted)
        {
            StateType = static_cast<uint8_t>(Value);
        }
    }
    if (StateType == 0xFF)
    {
        Log::Error(L"EFortItemEntryState::{} not found", StateName);
    }
    StateTypes.emplace(StateName, StateType);
    return StateType;
}

ptrdiff_t FFortItemEntry::DurabilityOffset()
{
    return Member_Durability.GetOffsetChecked();
}

int32_t FFortItemEntry::GetIntValue(uint8_t StateType) const
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

void FFortItemEntry::SetStateValue(uint8_t StateType, int32_t IntValue)
{
    if (StateType == 0xFF || (!Natives::FFortItemEntry_SetStateValue && !Finder::ResolveItemEntryStateLive()))
    {
        return;
    }
    Natives::FFortItemEntry_SetStateValue(this, StateType, IntValue);
}

bool FFortItemEntry::RemoveStateValue(uint8_t StateType)
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
    return GetIntValue(GetStateType(L"ShouldShowItemToast")) != 0;
}

void FFortItemEntry::SetNewItemCount(int32_t NewItemCount, bool bShouldShowItemToast)
{
#if WITH_SERVER_CODE
    SetStateValue(GetStateType(L"NewItemCount"), NewItemCount);
    if (bShouldShowItemToast)
    {
        SetStateValue(GetStateType(L"ShouldShowItemToast"), 1);
    }
#endif
}

void FFortItemEntry::AddNewItemCount(int32_t Count, bool bShouldShowItemToast)
{
    SetNewItemCount(GetIntValue(GetStateType(L"NewItemCount")) + Count, bShouldShowItemToast);
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

bool UFortItem::HasDurability() const
{
    Reflection::FParams Params(StaticFunction_HasDurability());
    return Params.Call(const_cast<UFortItem*>(this)) && Params.Take<bool>(L"ReturnValue");
}

float UFortWorldItemDefinition::GetMaxDurability(int32_t ItemLevel) const
{
    Reflection::FParams Params(StaticFunction_GetMaxDurability());
    Params.Set(L"ItemLevel", ItemLevel);
    return Params.Call(const_cast<UFortWorldItemDefinition*>(this)) ? Params.Take<float>(L"ReturnValue") : 1.0f;
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

    const uint8_t DurabilityInitialized = FFortItemEntry::GetStateType(L"DurabilityInitialized");
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
