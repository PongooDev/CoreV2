#include "pch.h"
#include "Player/FortPlayerControllerDeployableBase.h"
#include "Finder.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>
#include <tuple>
#include <vector>

class ADeployableBasePlot : public AActor
{
    DefineUnrealClass(ADeployableBasePlot, FortniteGame)
    DefineUFunction(IsOccupant)

public:
    DefineUProperty(AFortInventory*, PlotInventory)

    bool IsOccupant(const FUniqueNetIdRepl& UserId) const
    {
        Reflection::FParams Params(StaticFunction_IsOccupant(), true);
        Params.Set(L"UserId", UserId);
        return Params.Call(const_cast<ADeployableBasePlot*>(this)) && Params.Take<bool>(L"ReturnValue");
    }
};

namespace
{
    class AFortDeployableBaseManager : public AActor
    {
        DefineUnrealClass(AFortDeployableBaseManager, FortniteGame)
        DefineUFunction(GetCurrent)
        DefineUFunction(GetDeployableBasePlotFromLocation)

    public:
        static AFortDeployableBaseManager* GetCurrent(UObject* WorldContextObject)
        {
            Reflection::FParams Params(StaticFunction_GetCurrent());
            Params.Set(L"WorldContextObject", WorldContextObject);
            return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<AFortDeployableBaseManager*>(L"ReturnValue") : nullptr;
        }

        ADeployableBasePlot* GetDeployableBasePlotFromLocation(const FVector& Location) const
        {
            Reflection::FParams Params(StaticFunction_GetDeployableBasePlotFromLocation());
            Params.Set(L"Location", Location);
            return Params.Call(const_cast<AFortDeployableBaseManager*>(this)) ? Params.Take<ADeployableBasePlot*>(L"ReturnValue") : nullptr;
        }
    };

    class UFortDeployableBaseLibrary : public UObject
    {
        DefineUnrealClass(UFortDeployableBaseLibrary, FortniteGame)
        DefineUFunction(IsWorldItemDefValidToPersistForDeployableBasePlayerInventory)

    public:
        static bool IsWorldItemDefValidToPersistForDeployableBasePlayerInventory(const UFortWorldItemDefinition* ItemDefinition)
        {
            Reflection::FParams Params(StaticFunction_IsWorldItemDefValidToPersistForDeployableBasePlayerInventory());
            Params.Set(L"ItemDefinition", ItemDefinition);
            return Params.Call(StaticClass()->GetDefaultObject()) && Params.Take<bool>(L"ReturnValue");
        }
    };

    bool CanMoveToPlot(UFortWorldItem* WorldItem)
    {
        FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
        return ItemDefinition && !UFortDeployableBaseLibrary::IsWorldItemDefValidToPersistForDeployableBasePlayerInventory(ItemDefinition)
            && ItemDefinition->GetFilterCategory() != EFortInventoryFilter::GetInvisible();
    }

    std::vector<UFortWorldItem*> ItemsOf(AFortInventory* Inventory)
    {
        std::vector<UFortWorldItem*> Items;
        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num(); ++Index)
        {
            if (Instances[Index])
            {
                Items.push_back(Instances[Index]);
            }
        }
        return Items;
    }
}

ADeployableBasePlot* AFortPlayerControllerDeployableBase::GetPlotWithPermission(const ADeployableBasePlot* ExpectedPlot)
{
    AFortDeployableBaseManager* Manager = AFortDeployableBaseManager::GetCurrent(this);
    AFortPawn* Pawn = MyFortPawn;
    APlayerState* State = PlayerState;
    if (!Manager || !Pawn || !State)
    {
        return nullptr;
    }

    ADeployableBasePlot* Plot = Manager->GetDeployableBasePlotFromLocation(Pawn->GetTransform().GetLocation());
    return Plot && Plot == ExpectedPlot && Plot->IsOccupant(State->UniqueId) ? Plot : nullptr;
}

bool AFortPlayerControllerDeployableBase::MoveItemToPlot(ADeployableBasePlot* Plot, UFortWorldItem* WorldItem, int32_t Count)
{
    AFortInventory* PlotInventory = Plot->PlotInventory;
    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const int32_t Moving = std::min(Count, static_cast<int32_t>(ItemEntry.Count));
    if (!PlotInventory || Moving <= 0)
    {
        return false;
    }

    const FGuid ItemGuid = ItemEntry.ItemGuid;
    const int32_t Moved = PlotInventory->AddWorldItemCopy(this, ItemEntry, Moving, -1);
    return Moved > 0 && RemoveInventoryItem(ItemGuid, Moved, false);
}

bool AFortPlayerControllerDeployableBase::MoveItemFromPlot(ADeployableBasePlot* Plot, UFortWorldItem* PlotItem, int32_t Count)
{
    AFortInventory* PlotInventory = Plot->PlotInventory;
    AFortInventory* Inventory = WorldInventory;
    FFortItemEntry& ItemEntry = PlotItem->ItemEntry;
    const int32_t Moving = std::min(Count, static_cast<int32_t>(ItemEntry.Count));
    if (!PlotInventory || !Inventory || Moving <= 0)
    {
        return false;
    }

    std::vector<UFortWorldItem*> NewItems;
    const int32_t Moved = Inventory->AddWorldItemCopy(this, ItemEntry, Moving, -1, &NewItems);
    for (UFortWorldItem* NewItem : NewItems)
    {
        TryAddToQuickBar(NewItem);
    }
    PlotInventory->RemoveWorldItemCount(PlotItem, Moved);
    return Moved > 0;
}

void AFortPlayerControllerDeployableBase::ServerMoveItemToDeployableBase_Implementation(const FGuid& ItemOnPlayer, int32_t Count, const ADeployableBasePlot* ExpectedPlot)
{
#if WITH_SERVER_CODE
    ADeployableBasePlot* Plot = GetPlotWithPermission(ExpectedPlot);
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Plot && Inventory ? Inventory->FindWorldItem(ItemOnPlayer) : nullptr;
    if (WorldItem && CanMoveToPlot(WorldItem))
    {
        MoveItemToPlot(Plot, WorldItem, Count);
    }
#endif
}

void AFortPlayerControllerDeployableBase::ServerMoveItemFromDeployableBase_Implementation(const FGuid& ItemOnPlot, int32_t Count, const ADeployableBasePlot* ExpectedPlot)
{
#if WITH_SERVER_CODE
    ADeployableBasePlot* Plot = GetPlotWithPermission(ExpectedPlot);
    AFortInventory* PlotInventory = Plot ? static_cast<AFortInventory*>(Plot->PlotInventory) : nullptr;
    if (UFortWorldItem* PlotItem = PlotInventory ? PlotInventory->FindWorldItem(ItemOnPlot) : nullptr)
    {
        MoveItemFromPlot(Plot, PlotItem, Count);
    }
#endif
}

void AFortPlayerControllerDeployableBase::ServerMoveAllItemsToDeployableBase_Implementation(const ADeployableBasePlot* ExpectedPlot)
{
#if WITH_SERVER_CODE
    ADeployableBasePlot* Plot = GetPlotWithPermission(ExpectedPlot);
    AFortInventory* Inventory = WorldInventory;
    if (!Plot || !Inventory)
    {
        return;
    }

    for (UFortWorldItem* WorldItem : ItemsOf(Inventory))
    {
        if (CanMoveToPlot(WorldItem))
        {
            FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
            MoveItemToPlot(Plot, WorldItem, ItemEntry.Count);
        }
    }
#endif
}

void AFortPlayerControllerDeployableBase::ServerMoveAllItemsFromDeployableBase_Implementation(const ADeployableBasePlot* ExpectedPlot)
{
#if WITH_SERVER_CODE
    ADeployableBasePlot* Plot = GetPlotWithPermission(ExpectedPlot);
    AFortInventory* PlotInventory = Plot ? static_cast<AFortInventory*>(Plot->PlotInventory) : nullptr;
    if (!PlotInventory)
    {
        return;
    }

    for (UFortWorldItem* PlotItem : ItemsOf(PlotInventory))
    {
        FFortItemEntry& ItemEntry = PlotItem->ItemEntry;
        MoveItemFromPlot(Plot, PlotItem, ItemEntry.Count);
    }
#endif
}

void AFortPlayerControllerDeployableBase::HookVTables()
{
    UObject* ControllerDefault = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    void** ControllerVTable = ControllerDefault ? *reinterpret_cast<void***>(ControllerDefault) : nullptr;
    for (const auto& [Function, Implementation, Name] : {
             std::tuple{ StaticFunction_ServerMoveItemToDeployableBase(), reinterpret_cast<void*>(&ServerMoveItemToDeployableBase_ImplementationHK), L"AFortPlayerControllerDeployableBase::ServerMoveItemToDeployableBase_Implementation" },
             std::tuple{ StaticFunction_ServerMoveItemFromDeployableBase(), reinterpret_cast<void*>(&ServerMoveItemFromDeployableBase_ImplementationHK), L"AFortPlayerControllerDeployableBase::ServerMoveItemFromDeployableBase_Implementation" },
             std::tuple{ StaticFunction_ServerMoveAllItemsToDeployableBase(), reinterpret_cast<void*>(&ServerMoveAllItemsToDeployableBase_ImplementationHK), L"AFortPlayerControllerDeployableBase::ServerMoveAllItemsToDeployableBase_Implementation" },
             std::tuple{ StaticFunction_ServerMoveAllItemsFromDeployableBase(), reinterpret_cast<void*>(&ServerMoveAllItemsFromDeployableBase_ImplementationHK), L"AFortPlayerControllerDeployableBase::ServerMoveAllItemsFromDeployableBase_Implementation" },
         })
    {
        const int32_t Index = Finder::ImplementationIndex(Function);
        void* const Original = ControllerVTable && Index >= 0 ? ControllerVTable[Index] : nullptr;
        if (!Original)
        {
            Log::Error(L"Finder: {} slot not found", Name);
            continue;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Original)
            {
                Hooks::SwapVTable(Name, VTable, Index, Implementation);
            }
        }
    }
}

void AFortPlayerControllerDeployableBase::ServerMoveItemToDeployableBase_ImplementationHK(AFortPlayerControllerDeployableBase* This, const FGuid& ItemOnPlayer, int32_t Count, const ADeployableBasePlot* ExpectedPlot)
{
    This->ServerMoveItemToDeployableBase_Implementation(ItemOnPlayer, Count, ExpectedPlot);
}

void AFortPlayerControllerDeployableBase::ServerMoveItemFromDeployableBase_ImplementationHK(AFortPlayerControllerDeployableBase* This, const FGuid& ItemOnPlot, int32_t Count, const ADeployableBasePlot* ExpectedPlot)
{
    This->ServerMoveItemFromDeployableBase_Implementation(ItemOnPlot, Count, ExpectedPlot);
}

void AFortPlayerControllerDeployableBase::ServerMoveAllItemsToDeployableBase_ImplementationHK(AFortPlayerControllerDeployableBase* This, const ADeployableBasePlot* ExpectedPlot)
{
    This->ServerMoveAllItemsToDeployableBase_Implementation(ExpectedPlot);
}

void AFortPlayerControllerDeployableBase::ServerMoveAllItemsFromDeployableBase_ImplementationHK(AFortPlayerControllerDeployableBase* This, const ADeployableBasePlot* ExpectedPlot)
{
    This->ServerMoveAllItemsFromDeployableBase_Implementation(ExpectedPlot);
}
