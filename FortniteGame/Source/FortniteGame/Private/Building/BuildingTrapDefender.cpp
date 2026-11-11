#include "pch.h"
#include "Building/BuildingTrapDefender.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortAIPawn.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <tuple>
#include <vector>

namespace
{
    struct FSuppliedDefender
    {
        TWeakObjectPtr<ABuildingTrapDefender> Trap;
        TWeakObjectPtr<AFortAIPawn> Defender;
    };

    std::vector<FSuppliedDefender> SuppliedDefenders;
    bool bReturnQueued = false;

    // How often the supplied defenders are looked at, on a timer on the world's timer manager, while there are any.
    constexpr float ReturnCheckInterval = 0.25f;

    std::vector<UFortWorldItem*> FindItems(AFortInventory* Inventory, bool (*Matches)(UFortItemDefinition* Definition))
    {
        std::vector<UFortWorldItem*> Items;
        TArray<UFortWorldItem*>& Instances = static_cast<FFortItemList&>(Inventory->Inventory).ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num(); ++Index)
        {
            UFortWorldItem* Item = Instances[Index];
            if (Item && Matches(static_cast<UFortItemDefinition*>(static_cast<FFortItemEntry&>(Item->ItemEntry).ItemDefinition)))
            {
                Items.push_back(Item);
            }
        }
        return Items;
    }

    bool IsWeapon(UFortItemDefinition* Definition)
    {
        return Cast<UFortWeaponItemDefinition>(Definition) != nullptr;
    }

    bool IsAmmo(UFortItemDefinition* Definition)
    {
        return Cast<UFortAmmoItemDefinition>(Definition) != nullptr;
    }
}

void ABuildingTrapDefender::SupplyDefender(const UFortWorldItem* WeaponToGive, int32_t AmmoQuantity)
{
#if WITH_SERVER_CODE
    AFortAIPawn* Defender = DefenderPawn;
    AFortInventory* DefenderInventory = Defender ? Defender->GetWorldInventory() : nullptr;
    if (Role != ROLE_Authority || !Defender)
    {
        return;
    }
    if (!DefenderInventory)
    {
        Log::Warning(L"Defender: {} at {} has no inventory to supply", Defender->GetName().ToStdString(), GetName().ToStdString());
        return;
    }

    // A defender this one replaced gives its weapon and ammo back first, so its ammo can go to this one.
    ReturnLeftDefendersItems();
    WatchDefender(this, Defender);

    AFortPlayerController* Manager = Defender->GetPlayerManagerController();
    AFortInventory* ManagerInventory = Manager ? static_cast<AFortInventory*>(Manager->WorldInventory) : nullptr;

    // The weapon: the defender's own, or the player's, moved into the defender's inventory.
    UFortWorldItem* Weapon = const_cast<UFortWorldItem*>(WeaponToGive);
    AFortInventory* WeaponInventory = Weapon ? static_cast<AFortInventory*>(Weapon->OwnerInventory) : nullptr;
    UFortWeaponItemDefinition* WeaponDefinition = Weapon ? Cast<UFortWeaponItemDefinition>(static_cast<UFortItemDefinition*>(static_cast<FFortItemEntry&>(Weapon->ItemEntry).ItemDefinition)) : nullptr;
    if (Weapon && (!WeaponDefinition || (WeaponInventory != DefenderInventory && (!ManagerInventory || WeaponInventory != ManagerInventory))))
    {
        Log::Warning(L"Defender: {} isn't a weapon of {} or of its player, {} gets none", Weapon->GetName().ToStdString(), Defender->GetName().ToStdString(), Defender->GetName().ToStdString());
        Weapon = nullptr;
        WeaponDefinition = nullptr;
    }
    else if (Weapon && WeaponInventory != DefenderInventory)
    {
        FFortItemEntry& Entry = Weapon->ItemEntry;
        const FGuid PlayerGuid = Entry.ItemGuid;
        std::vector<UFortWorldItem*> NewItems;
        DefenderInventory->AddWorldItemCopy(nullptr, Entry, Entry.Count, 1, &NewItems);
        Weapon = NewItems.empty() ? nullptr : NewItems.front();
        if (Weapon)
        {
            Manager->RemoveInventoryItem(PlayerGuid, -1, true);
        }
    }

    // Any other weapon it held goes back to the player.
    for (UFortWorldItem* Item : FindItems(DefenderInventory, &IsWeapon))
    {
        if (Item != Weapon)
        {
            Defender->ReturnItemToPlayerManager(Item, static_cast<FFortItemEntry&>(Item->ItemEntry).Count);
        }
    }

    AFortWeapon* OldWeapon = Defender->CurrentWeapon;
    if (Weapon)
    {
        AFortWeapon* NewWeapon = Defender->EquipWeaponDefinition(WeaponDefinition, static_cast<FFortItemEntry&>(Weapon->ItemEntry).ItemGuid);
        if (NewWeapon && OldWeapon && NewWeapon != OldWeapon)
        {
            Defender->RemoveAbilityWeapon(OldWeapon);
        }
    }

    // The ammo: the weapon's own ammo up to AmmoQuantity; ammo for anything else goes back.
    UFortWorldItemDefinition* AmmoDefinition = WeaponDefinition ? static_cast<TAssetPtr<UFortWorldItemDefinition>&>(WeaponDefinition->AmmoData).LoadSynchronous() : nullptr;
    int32_t AmmoHeld = 0;
    for (UFortWorldItem* Item : FindItems(DefenderInventory, &IsAmmo))
    {
        FFortItemEntry& Entry = Item->ItemEntry;
        if (AmmoDefinition && static_cast<UFortItemDefinition*>(Entry.ItemDefinition) == AmmoDefinition)
        {
            AmmoHeld += Entry.Count;
        }
        else
        {
            Defender->ReturnItemToPlayerManager(Item, Entry.Count);
        }
    }

    const int32_t AmmoWanted = std::max(AmmoQuantity, 0);
    if (AmmoDefinition && AmmoWanted > AmmoHeld && Manager)
    {
        const int32_t Taken = UFortKismetLibrary::K2_RemoveItemFromPlayer(Manager, AmmoDefinition, std::min(AmmoWanted - AmmoHeld, Manager->GetItemQuantity(AmmoDefinition)));
        if (Taken > 0)
        {
            UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
            std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
            FFortItemEntry* AmmoEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
            FFortItemEntry::Construct(AmmoEntry, AmmoDefinition, Taken, 0);
            AmmoHeld += DefenderInventory->AddWorldItemCopy(nullptr, *AmmoEntry, Taken, -1);
            EntryStruct->DestroyStruct(AmmoEntry);
        }
    }
    else if (AmmoDefinition && AmmoWanted < AmmoHeld)
    {
        int32_t Excess = AmmoHeld - AmmoWanted;
        for (UFortWorldItem* Item : FindItems(DefenderInventory, &IsAmmo))
        {
            FFortItemEntry& Entry = Item->ItemEntry;
            if (Excess > 0 && static_cast<UFortItemDefinition*>(Entry.ItemDefinition) == AmmoDefinition)
            {
                const int32_t Returned = std::min(Excess, static_cast<int32_t>(Entry.Count));
                Defender->ReturnItemToPlayerManager(Item, Returned);
                Excess -= Returned;
                AmmoHeld -= Returned;
            }
        }
    }

    Log::Info(L"Defender: {} at {} holds {} and {} {} (asked for {})", Defender->GetName().ToStdString(), GetName().ToStdString(),
        WeaponDefinition && Weapon ? WeaponDefinition->GetName().ToStdString() : L"no weapon", AmmoHeld,
        AmmoDefinition ? AmmoDefinition->GetName().ToStdString() : L"ammo", AmmoQuantity);
#endif
}

UFortWorldItem* ABuildingTrapDefender::TakeWeaponAwayFromDefender(UFortWorldItem* WeaponToTakeAway)
{
#if WITH_SERVER_CODE
    AFortAIPawn* Defender = DefenderPawn;
    AFortInventory* DefenderInventory = Defender ? Defender->GetWorldInventory() : nullptr;
    if (Role != ROLE_Authority || !WeaponToTakeAway || !DefenderInventory || static_cast<AFortInventory*>(WeaponToTakeAway->OwnerInventory) != DefenderInventory)
    {
        return WeaponToTakeAway;
    }
    return Defender->ReturnItemToPlayerManager(WeaponToTakeAway, static_cast<FFortItemEntry&>(WeaponToTakeAway->ItemEntry).Count);
#else
    return WeaponToTakeAway;
#endif
}

void ABuildingTrapDefender::WatchDefender(ABuildingTrapDefender* Trap, AFortAIPawn* Defender)
{
    const TWeakObjectPtr<AFortAIPawn> WeakDefender(Defender);
    if (std::none_of(SuppliedDefenders.begin(), SuppliedDefenders.end(), [&](const FSuppliedDefender& Supplied) { return Supplied.Defender == WeakDefender; }))
    {
        SuppliedDefenders.push_back({ TWeakObjectPtr<ABuildingTrapDefender>(Trap), WeakDefender });
    }
    QueueReturnCheck(Trap ? Trap->GetWorld() : nullptr);
}

void ABuildingTrapDefender::QueueReturnCheck(UWorld* World)
{
    if (bReturnQueued || !World)
    {
        return;
    }
    bReturnQueued = true;
    FTimerHandle ReturnTimerHandle;
    World->GetTimerManager().SetTimer(ReturnTimerHandle, FTimerDelegate::CreateLambda([]() { ReturnLeftDefendersItems(); }), ReturnCheckInterval, false);
}

void ABuildingTrapDefender::ReturnLeftDefendersItems()
{
    bReturnQueued = false;
    UWorld* World = nullptr;
    for (auto It = SuppliedDefenders.begin(); It != SuppliedDefenders.end();)
    {
        // A defender destroyed this tick (its post's force kill) is still readable until garbage collection.
        AFortAIPawn* Defender = It->Defender.Get(true);
        ABuildingTrapDefender* Trap = It->Trap.Get();
        if (Defender && Trap && !Defender->IsPendingKill() && static_cast<AFortAIPawn*>(Trap->DefenderPawn) == Defender)
        {
            World = World ? World : Trap->GetWorld();
            ++It;
            continue;
        }
        if (Defender)
        {
            Defender->ReturnInventoryToPlayerManager();
        }
        It = SuppliedDefenders.erase(It);
    }
    if (!SuppliedDefenders.empty())
    {
        QueueReturnCheck(World);
    }
}

DEFINE_FUNCTION(ABuildingTrapDefender::execSupplyDefender)
{
    P_GET_OBJECT(UFortWorldItem, Z_Param_WeaponToGive);
    P_GET_PROPERTY(UIntProperty, Z_Param_AmmoQuantity);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<ABuildingTrapDefender*>(Context)->SupplyDefender(Z_Param_WeaponToGive, Z_Param_AmmoQuantity);
    P_NATIVE_END;
}

DEFINE_FUNCTION(ABuildingTrapDefender::execTakeWeaponAwayFromDefender)
{
    P_GET_OBJECT(UFortWorldItem, Z_Param_WeaponToTakeAway);
    P_FINISH;
    P_NATIVE_BEGIN;
    *static_cast<UFortWorldItem**>(RESULT_PARAM) = static_cast<ABuildingTrapDefender*>(Context)->TakeWeaponAwayFromDefender(Z_Param_WeaponToTakeAway);
    P_NATIVE_END;
}

void ABuildingTrapDefender::HookNatives()
{
    if (!Natives::FFrame_Step && !Finder::ResolveScriptFrame())
    {
        return;
    }
    for (const auto& [Function, Native, Name] : {
             std::tuple{ FindStaticFunction_SupplyDefender(), &execSupplyDefender, L"ABuildingTrapDefender::SupplyDefender" },
             std::tuple{ FindStaticFunction_TakeWeaponAwayFromDefender(), &execTakeWeaponAwayFromDefender, L"ABuildingTrapDefender::TakeWeaponAwayFromDefender" } })
    {
        uint8_t* NativeCall = Function ? Finder::ImplementationCall(Function) : nullptr;
        if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
        {
            Log::Info(L"Finder: {} is native in this build", Name);
            continue;
        }
        Function->SetNativeFunc(Native);
        Log::Info(L"Hook {}: native", Name);
    }
}
