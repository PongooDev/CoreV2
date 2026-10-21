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
#include "UObject/UObjectIterator.h"
#include "Natives.h"
#include "ReflectionParams.h"

UFortItem* UFortItemDefinition::CreateTemporaryItemInstanceBP(int32_t Count, int32_t Level) const
{
    Reflection::FParams Params(StaticFunction_CreateTemporaryItemInstanceBP());
    Params.Set(L"Count", Count);
    Params.Set(L"Level", Level);
    return Params.Call(const_cast<UFortItemDefinition*>(this)) ? Params.Take<UFortItem*>(L"ReturnValue") : nullptr;
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

bool UFortWorldItemDefinition::ServerExecute(const UFortItem* Item, AFortPlayerController* Controller)
{
    if (Natives::UFortWorldItemDefinition_ServerExecuteIndex < 0 && !Finder::ResolveWorldItemDefinitionLive())
    {
        return false;
    }

    using FServerExecute = bool (*)(UFortWorldItemDefinition* This, const UFortItem* Item, AFortPlayerController* Controller);
    return reinterpret_cast<FServerExecute>((*reinterpret_cast<void* const* const*>(this))[Natives::UFortWorldItemDefinition_ServerExecuteIndex])(this, Item, Controller);
}

void UFortWorldItemDefinition::OnItemAddedToInventory(void* InventoryOwner, UFortWorldItem* Item, bool bSkipQuickBar)
{
    if (Natives::UFortWorldItemDefinition_ServerExecuteIndex < 0 && !Finder::ResolveWorldItemDefinitionLive())
    {
        return;
    }
    if (Natives::UFortWorldItemDefinition_OnItemAddedToInventoryIndex < 0)
    {
        return;
    }

    using FOnItemAddedToInventory = void (*)(UFortWorldItemDefinition* This, void* InventoryOwner, UFortWorldItem* Item, bool bSkipQuickBar);
    reinterpret_cast<FOnItemAddedToInventory>((*reinterpret_cast<void* const* const*>(this))[Natives::UFortWorldItemDefinition_OnItemAddedToInventoryIndex])(this, InventoryOwner, Item, bSkipQuickBar);
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

void UFortWorldItem::SetOwningControllerForTemporaryItem(AFortPlayerController* InController)
{
    Reflection::FParams Params(StaticFunction_SetOwningControllerForTemporaryItem());
    Params.Set(L"InController", InController);
    Params.Call(this);
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
}

bool UFortWorldItem::SetLoadedAmmoHK(UFortWorldItem* This, int32_t NewLoadedAmmo)
{
    return This->SetLoadedAmmo(NewLoadedAmmo);
}
