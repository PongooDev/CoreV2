#include "pch.h"
#include "Items/FortQuickBars.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "Weapons/FortWeapon.h"
#include "UObject/UObjectIterator.h"
#include "UObject/Class.h"

EFortQuickBars AFortQuickBars::GetCurrentQuickBar()
{
    for (EFortQuickBars InQuickBar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        FQuickBar& QuickBar = GetQuickBar(InQuickBar);
        if (QuickBar.CurrentFocusedSlot != -1)
        {
            return InQuickBar;
        }
    }
    return EFortQuickBars::GetMax_None();
}

void AFortQuickBars::SwapQuickBarFocus(EFortQuickBars InQuickBar, int32_t SlotOverride)
{
    if (Natives::AFortQuickBars_SwapQuickBarFocus || Finder::ResolveQuickBarFocusLive())
    {
        Natives::AFortQuickBars_SwapQuickBarFocus(this, static_cast<int32_t>(InQuickBar), SlotOverride);
    }
}

int32_t AFortQuickBars::FocusSlot(EFortQuickBars InQuickBar, int32_t Slot, bool bUpdatePreviousFocusedSlot)
{
    if (Natives::AFortQuickBars_FocusSlot || Finder::ResolveQuickBarFocusLive())
    {
        return Natives::AFortQuickBars_FocusSlot(this, static_cast<int32_t>(InQuickBar), Slot, bUpdatePreviousFocusedSlot);
    }
    return -1;
}

void AFortQuickBars::EnableSlot(EFortQuickBars InQuickBar, int32_t SlotIndex)
{
    Reflection::FParams Params(StaticFunction_EnableSlot());
    Params.Set(L"InQuickBar", static_cast<uint8_t>(InQuickBar));
    Params.Set(L"SlotIndex", SlotIndex);
    Params.Call(this);
}

void AFortQuickBars::ServerActivateSlotInternal(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot)
{
    Reflection::FParams Params(StaticFunction_ServerActivateSlotInternal());
    Params.Set(L"InQuickBar", static_cast<uint8_t>(InQuickBar));
    Params.Set(L"Slot", Slot);
    Params.Set(L"ActivateDelay", ActivateDelay);
    Params.Set(L"bUpdatePreviousFocusedSlot", bUpdatePreviousFocusedSlot);
    Params.Call(this);
}

void AFortQuickBars::ServerAddItemInternal(const FGuid& Item, EFortQuickBars InQuickBar, int32_t Slot)
{
    Reflection::FParams Params(StaticFunction_ServerAddItemInternal());
    Params.Set(L"Item", Item);
    Params.Set(L"InQuickBar", static_cast<uint8_t>(InQuickBar));
    Params.Set(L"Slot", Slot);
    Params.Call(this);
}

FQuickBarSlot& AFortQuickBars::GetSlot(EFortQuickBars InQuickBar, int32_t SlotIndex)
{
    TStructArray<FQuickBarSlot>& Slots = GetQuickBar(InQuickBar).Slots;
    return Slots[SlotIndex];
}

const FQuickBarSlotData& AFortQuickBars::GetSlotData(EFortQuickBars InQuickBar, int32_t SlotIndex)
{
    FQuickBarData& DataDefinition = GetQuickBar(InQuickBar).DataDefinition;
    TStructArray<FQuickBarSlotData>& SlotData = DataDefinition.QuickbarSlots;
    return SlotData[SlotIndex];
}

bool AFortQuickBars::ContainsItem(const FGuid& Item)
{
    for (EFortQuickBars Bar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        const int32_t NumSlots = GetQuickBar(Bar).Slots.Num();
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            if (GetSlot(Bar, SlotIndex).Items.Contains(Item))
            {
                return true;
            }
        }
    }
    return false;
}

void AFortQuickBars::ServerRemoveItemInternal(const FGuid& Item, bool bFindReplacement, bool bForce)
{
    Reflection::FParams Params(StaticFunction_ServerRemoveItemInternal());
    Params.Set(L"Item", Item);
    Params.Set(L"bFindReplacement", bFindReplacement);
    Params.Set(L"bForce", bForce);
    Params.Call(this);
}

void AFortQuickBars::ServerRemoveItemInternal_Implementation(const FGuid& Item, bool bFindReplacement, bool bForce)
{
#if WITH_SERVER_CODE
    bool bRemovedFocusedItem = false;
    EFortQuickBars FocusedBar = EFortQuickBars::GetPrimary();
    for (EFortQuickBars Bar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        FQuickBar& QuickBar = GetQuickBar(Bar);
        const int32_t NumSlots = QuickBar.Slots.Num();
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            TArray<FGuid>& Items = GetSlot(Bar, SlotIndex).Items;
            for (int32_t Index = Items.Num() - 1; Index >= 0; --Index)
            {
                if (Items[Index] == Item)
                {
                    Items.RemoveAt(Index);
                    if (SlotIndex == QuickBar.CurrentFocusedSlot && Items.Num() == 0)
                    {
                        bRemovedFocusedItem = true;
                        FocusedBar = Bar;
                    }
                }
            }
        }
    }

    AFortPlayerController* Controller = Cast<AFortPlayerController>(GetOwner());
    AFortPawn* Pawn = Controller ? static_cast<AFortPawn*>(Controller->MyFortPawn) : nullptr;
    AFortWeapon* CurrentWeapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    const bool bWeaponReactivatesSlot = CurrentWeapon && CurrentWeapon->IsPendingDestroyDueToDurability() && static_cast<FGuid&>(CurrentWeapon->ItemEntryGuid) == Item;
    if (bRemovedFocusedItem && KeepFocusOnRemoveCount > 0)
    {
        Log::Info(L"Quickbar: {} keeps slot {} focused while its item is replaced", GetNameSafe(Controller).ToStdString(), static_cast<int32_t>(GetQuickBar(FocusedBar).CurrentFocusedSlot));
    }
    else if (bRemovedFocusedItem && bWeaponReactivatesSlot)
    {
        FQuickBar& QuickBar = GetQuickBar(FocusedBar);
        const int32_t NumSlots = QuickBar.Slots.Num();
        const int32_t EmptiedSlot = QuickBar.CurrentFocusedSlot;
        for (int32_t Step = 1; Step < NumSlots; ++Step)
        {
            if (FocusSlot(FocusedBar, (EmptiedSlot + Step) % NumSlots, true) != -1)
            {
                break;
            }
        }
    }
    else if (bRemovedFocusedItem)
    {
        ActivateReplacementSlot();
    }
#endif
}

void AFortQuickBars::ActivateReplacementSlot()
{
#if WITH_SERVER_CODE
    // The primary bar's previously focused slot if it still holds an item, otherwise the harvesting tool's (slot 0).
    FQuickBar& PrimaryBar = PrimaryQuickBar;
    const int32_t PreviousSlot = PrimaryBar.PreviousFocusedSlot;
    const bool bUsePrevious = PreviousSlot >= 0 && PreviousSlot < PrimaryBar.Slots.Num() && PreviousSlot != PrimaryBar.CurrentFocusedSlot && GetSlot(EFortQuickBars::GetPrimary(), PreviousSlot).Items.Num() > 0;
    const int32_t Slot = bUsePrevious ? PreviousSlot : 0;
    Log::Info(L"Quickbar: {} activates slot {} after its focused item was removed", GetNameSafe(GetOwner()).ToStdString(), Slot);
    ServerActivateSlotInternal(EFortQuickBars::GetPrimary(), Slot, 0.0f, false);
#endif
}

void AFortQuickBars::HookVTables()
{
    const int32_t RemoveIndex = Finder::ImplementationIndex(StaticFunction_ServerRemoveItemInternal());
    if (RemoveIndex < 0)
    {
        Log::Error(L"Finder: AFortQuickBars::ServerRemoveItemInternal_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[RemoveIndex] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortQuickBars::ServerRemoveItemInternal_Implementation", VTable, RemoveIndex, reinterpret_cast<void*>(&ServerRemoveItemInternal_ImplementationHK));
        }
    }
}

void AFortQuickBars::ServerRemoveItemInternal_ImplementationHK(AFortQuickBars* This, const FGuid& Item, bool bFindReplacement, bool bForce)
{
    This->ServerRemoveItemInternal_Implementation(Item, bFindReplacement, bForce);
}

void AFortQuickBars::SetupQuickbar(EFortQuickBars InQuickBar, const FQuickBarData& QuickBarData)
{
#if WITH_SERVER_CODE
    FQuickBar& QuickBar = GetQuickBar(InQuickBar);
    FQuickBarData& DataDefinition = QuickBar.DataDefinition;
    FQuickBarData::StaticStruct()->CopyScriptStruct(&DataDefinition, &QuickBarData);

    TStructArray<FQuickBarSlot>& Slots = QuickBar.Slots;
    const int32_t NumSlots = DataDefinition.QuickbarSlots.Num();
    while (Slots.Num() < NumSlots)
    {
        Slots.AddDefaulted();
    }

    for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
    {
        EnableSlot(InQuickBar, SlotIndex);
    }
#endif
}
