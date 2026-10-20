#pragma once

#include "Containers/Array.h"
#include "StructArray.h"
#include "GameFramework/Actor.h"
#include "Misc/Guid.h"

enum class EFortQuickBars : uint8_t
{
    Primary,
    Secondary,
    Max_None,
};

struct FQuickBarSlot
{
    DefineUnrealStruct(FQuickBarSlot, FortniteGame)
    DefineUProperty(TArray<FGuid>, Items)
};

struct FQuickBarSlotData
{
    DefineUnrealStruct(FQuickBarSlotData, FortniteGame)
    DefineUProperty(TArray<uint8_t>, AcceptedItemTypes)
};

struct FQuickBarData
{
    DefineUnrealStruct(FQuickBarData, FortniteGame)
    DefineUStructArrayProperty(FQuickBarSlotData, QuickbarSlots)
};

struct FQuickBar
{
    DefineUnrealStruct(FQuickBar, FortniteGame)
    DefineUStructArrayProperty(FQuickBarSlot, Slots)
    DefineUStructProperty(FQuickBarData, DataDefinition)
};

class AFortQuickBars : public AActor
{
    DefineUnrealClass(AFortQuickBars, FortniteGame)

public:
    DefineUStructProperty(FQuickBar, PrimaryQuickBar)
    DefineUStructProperty(FQuickBar, SecondaryQuickBar)
    DefineUFunction(EnableSlot)
    DefineUFunction(ServerAddItemInternal)

    FQuickBar& GetQuickBar(EFortQuickBars InQuickBar) { return InQuickBar == EFortQuickBars::Primary ? PrimaryQuickBar : SecondaryQuickBar; }

    void EnableSlot(EFortQuickBars InQuickBar, int32_t SlotIndex);
    void ServerAddItemInternal(const FGuid& Item, EFortQuickBars InQuickBar, int32_t Slot);
    FQuickBarSlot& GetSlot(EFortQuickBars InQuickBar, int32_t SlotIndex);
    const FQuickBarSlotData& GetSlotData(EFortQuickBars InQuickBar, int32_t SlotIndex);
    bool ContainsItem(const FGuid& Item);
    void RemoveItem(const FGuid& Item);
    void SetupQuickbar(EFortQuickBars InQuickBar, const FQuickBarData& QuickBarData);
};
