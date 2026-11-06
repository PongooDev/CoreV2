#include "pch.h"
#include "Pawns/FortAIPawn.h"
#include "AI/FortAIDirector.h"
#include "BlueprintGameplayTagLibrary.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "FortRuntimeOptions.h"
#include "Hooks.h"
#include "Engine/World.h"
#include "Kismet/KismetMathLibrary.h"
#include "Math/Rotator.h"
#include "Items/FortInventory.h"
#include "Items/FortInventoryOwnerInterface.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Log.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "Player/FortPlayerState.h"
#include "Templates/Casts.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <vector>
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

IFortInventoryOwnerInterface* AFortAIPawn::GetInventoryOwner()
{
    return InventoryOwnerInterfaceOffset >= 0 ? reinterpret_cast<IFortInventoryOwnerInterface*>(reinterpret_cast<uint8_t*>(this) + InventoryOwnerInterfaceOffset) : nullptr;
}

AFortInventory* AFortAIPawn::GetWorldInventory()
{
    IFortInventoryOwnerInterface* InventoryOwner = GetInventoryOwner();
    return InventoryOwner && Natives::IFortInventoryOwnerInterface_GetWorldInventoryIndex >= 0 ? InventoryOwner->GetWorldInventory() : nullptr;
}

AFortPlayerController* AFortAIPawn::GetPlayerManagerController()
{
    AFortPlayerStateZone* Manager = Member_PlayerManager.IsAvailable() ? static_cast<AFortPlayerStateZone*>(PlayerManager) : nullptr;
    return Manager ? Cast<AFortPlayerController>(Manager->GetOwner()) : nullptr;
}

void AFortAIPawn::ReturnInventoryToPlayerManager()
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = GetWorldInventory();
    if (Role != ROLE_Authority || !Inventory)
    {
        return;
    }

    // What a post supplies or a defender picks up: its weapon and the ammo for it.
    std::vector<UFortWorldItem*> Items;
    TArray<UFortWorldItem*>& Instances = static_cast<FFortItemList&>(Inventory->Inventory).ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        UFortWorldItem* Item = Instances[Index];
        UFortItemDefinition* Definition = Item ? static_cast<UFortItemDefinition*>(static_cast<FFortItemEntry&>(Item->ItemEntry).ItemDefinition) : nullptr;
        if (Cast<UFortWeaponItemDefinition>(Definition) || Cast<UFortAmmoItemDefinition>(Definition))
        {
            Items.push_back(Item);
        }
    }
    if (Items.empty())
    {
        return;
    }

    for (UFortWorldItem* Item : Items)
    {
        ReturnItemToPlayerManager(Item, static_cast<FFortItemEntry&>(Item->ItemEntry).Count);
    }
#endif
}

UFortWorldItem* AFortAIPawn::ReturnItemToPlayerManager(UFortWorldItem* Item, int32_t Count)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = GetWorldInventory();
    if (Role != ROLE_Authority || !Inventory || !Item || static_cast<AFortInventory*>(Item->OwnerInventory) != Inventory)
    {
        return nullptr;
    }

    FFortItemEntry& Entry = Item->ItemEntry;
    Count = std::clamp(Count, 0, static_cast<int32_t>(Entry.Count));
    if (Count <= 0)
    {
        return nullptr;
    }

    // A weapon leaving the defender's hands is unequipped first.
    AFortWeapon* Weapon = CurrentWeapon;
    if (Weapon && static_cast<FGuid>(Weapon->ItemEntryGuid) == static_cast<FGuid>(Entry.ItemGuid))
    {
        RemoveAbilityWeapon(Weapon);
    }

    AFortPlayerController* Manager = GetPlayerManagerController();
    AFortInventory* ManagerInventory = Manager ? static_cast<AFortInventory*>(Manager->WorldInventory) : nullptr;
    UFortWorldItem* Returned = nullptr;
    int32_t Given = 0;
    if (ManagerInventory)
    {
        std::vector<UFortWorldItem*> NewItems;
        Given = ManagerInventory->AddWorldItemCopy(Manager, Entry, Count, -1, &NewItems);
        for (UFortWorldItem* NewItem : NewItems)
        {
            Manager->TryAddToQuickBar(NewItem);
        }
        Returned = NewItems.empty() ? nullptr : NewItems.front();
    }
    if (Given < Count)
    {
        UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
        std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
        FFortItemEntry* DropEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
        EntryStruct->InitializeStruct(DropEntry);
        EntryStruct->CopyScriptStruct(DropEntry, &Entry);
        DropEntry->Count = Count - Given;
        const FVector Location = GetTransform().GetLocation();
        if (AFortPickup* Pickup = AFortPickup::CreatePickup(GetWorld(), *DropEntry, &Location, nullptr, Manager))
        {
            Pickup->TossPickup(Location, this, 0);
        }
        EntryStruct->DestroyStruct(DropEntry);
    }

    const UFortItemDefinition* Definition = Entry.ItemDefinition;
    Log::Info(L"Defender: {} gives {} {} back to {} ({} dropped)", GetName().ToStdString(), Count, Definition ? Definition->GetName().ToStdString() : L"None",
        Manager ? Manager->GetName().ToStdString() : L"no one", Count - Given);
    if (Count >= Entry.Count)
    {
        Inventory->RemoveWorldItem(Item);
    }
    else
    {
        Inventory->UpdateWorldItemCount(Item, Entry.Count - Count);
    }
    return Returned;
#else
    return nullptr;
#endif
}

bool AFortAIPawn::RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = GetWorldInventory();
    UFortWorldItem* WorldItem = Role == ROLE_Authority && Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem)
    {
        return false;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const int32_t Remaining = ItemEntry.Count - Count;
    if (Count < 0 || Remaining <= 0)
    {
        Inventory->RemoveWorldItem(WorldItem);
    }
    else
    {
        Inventory->UpdateWorldItemCount(WorldItem, Remaining);
    }
    return true;
#else
    return false;
#endif
}

void AFortAIPawn::DropLoot(const FFortAIPawnLootDropData& LootDropData, const FGameplayTagContainer& DamageTags)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || bHasDroppedLoot)
    {
        return;
    }
    AFortAIDirector* Director = AFortAIDirector::GetCurrent(this);
    const FGameplayTagQuery* DenialQuery = Director ? Director->GetLootDropDenialQuery() : nullptr;
    if (DenialQuery && !DenialQuery->IsEmpty() && UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(DamageTags, *DenialQuery))
    {
        return;
    }
    bHasDroppedLoot = true;

    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const int32_t WorldLevel = GameState ? static_cast<int32_t>(GameState->WorldLevel) : 0;
    const FVector Location = GetTransform().GetLocation();
    const FRotator Rotation(0.0f, 0.0f, 0.0f);
    const float ConeHalfAngle = FMath::Max(static_cast<float>(LootDropConeHalfAngle), 0.0f) * 0.017453292f;
    const float Speed = LootDropSpeed;

    // Each drop leaves the body upwards, within the cone, at the pawn's loot drop speed.
    const auto DropFromTierGroup = [&](FName TierGroup, float Chance, const wchar_t* Kind)
    {
        if (TierGroup.IsNone() || Chance <= 0.0f || FMath::FRand() >= Chance)
        {
            return;
        }
        TStructArray<FFortItemEntry> LootDrops{};
        UFortKismetLibrary::PickLootDrops(LootDrops, TierGroup, WorldLevel, -1);
        int32_t Dropped = 0;
        for (int32_t Index = 0; Index < LootDrops.Num(); ++Index)
        {
            FFortItemEntry& Entry = LootDrops[Index];
            if (!Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry.ItemDefinition)))
            {
                continue;
            }
            if (AFortPickup* Pickup = AFortPickup::CreatePickup(World, Entry, &Location, &Rotation))
            {
                const FVector Direction = UKismetMathLibrary::RandomUnitVectorInCone(FVector(0.0f, 0.0f, 1.0f), ConeHalfAngle);
                Pickup->TossFromContainer(Location, Direction * Speed);
                ++Dropped;
            }
        }
        LootDrops.Empty();
        Log::Info(L"AFortAIPawn: {} dropped {} {} item(s) from {} (world level {})", GetName().ToStdString(), Dropped, Kind, TierGroup.ToString().ToStdString(), WorldLevel);
    };
    DropFromTierGroup(LootDropData.WorldItemTierGroup, LootDropData.WorldItemLootDropChance, L"world");
#endif
}

void AFortAIPawn::HandleDeathHK(AFortAIPawn* This, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, const void* HitInfo, void* Arg5)
{
    // The handler's own tests (4.0 0xEEBAC0): nothing for a pawn already down or dying, and a pawn that uses DBNO is
    // knocked down instead. The loot drop data is worked out first: the encounter's death handling, which the handler
    // runs, can let the pawn's spawn group go.
    UFortAIEncounterInfo* Encounter = !This->bIsDBNO && !This->bIsDying && !This->bUsesDBNO ? static_cast<UFortAIEncounterInfo*>(This->EncounterInfo) : nullptr;
    FFortAIPawnLootDropData LootDropData;
    if (Encounter)
    {
        FGameplayTagContainer AITags;
        Encounter->GetAIPawnLootDropTags(This, AITags);
        Encounter->GetAIPawnLootDropData(AITags, LootDropData);
    }
    HandleDeathOG(This, Damage, DamageTags, Momentum, HitInfo, Arg5);
    if (Encounter)
    {
        This->DropLoot(LootDropData, DamageTags);
    }
}

void AFortAIPawn::HookDeathVTables()
{
#if WITH_SERVER_CODE
    // Enemy loot drops came with 4.0's loot drop rows; earlier builds have none to drop.
    if (!UFortAIEncounterInfo::HasLootDropRows() || !Member_bHasDroppedLoot.IsAvailable() || !Member_bUsesDBNO.IsAvailable() || !Member_EncounterInfo.IsAvailable())
    {
        return;
    }
    UObject* PawnDefault = StaticClass()->GetDefaultObject(false);
    void** PawnVTable = PawnDefault ? *reinterpret_cast<void***>(PawnDefault) : nullptr;
    // The mask is only set once the offset resolves, and the arguments are evaluated right to left.
    const ptrdiff_t UsesDBNOOffset = Member_bUsesDBNO.GetOffsetChecked();
    if (!PawnVTable || !Finder::ResolveAIPawnDeathLive(PawnVTable, UsesDBNOOffset, Member_bUsesDBNO.GetFieldMask()))
    {
        return;
    }

    const int32_t DeathIndex = Natives::AFortAIPawn_HandleDeathIndex;
    void* const PawnHandleDeath = PawnVTable[DeathIndex];
    HandleDeathOG = reinterpret_cast<decltype(HandleDeathOG)>(PawnHandleDeath);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[DeathIndex] == PawnHandleDeath)
        {
            Hooks::SwapVTable(L"AFortAIPawn death handler (loot drops)", VTable, DeathIndex, reinterpret_cast<void*>(&HandleDeathHK));
        }
    }

    // The encounters only get loot drop rows with bEnableSTWLootDrops on.
    const UFortRuntimeOptions* Options = UFortRuntimeOptions::Get();
    Log::Info(L"AFortAIPawn: enemy loot drops restored (bEnableSTWLootDrops {})", Options && UFortRuntimeOptions::HasSTWLootDrops() && Options->bEnableSTWLootDrops);
#endif
}

void AFortAIPawn::HookVTables()
{
    HookDeathVTables();

    UObject* PawnDefault = StaticClass()->GetDefaultObject(false);
    void* InventoryOwner = PawnDefault && Natives::UObject_GetInterfaceAddress ? Natives::UObject_GetInterfaceAddress(PawnDefault, UFortInventoryOwnerInterface::StaticClass()) : nullptr;
    const int32_t RemoveIndex = Natives::IFortInventoryOwnerInterface_RemoveInventoryItemIndex;
    if (!InventoryOwner || RemoveIndex < 0)
    {
        Log::Error(L"Finder: AFortAIPawn's IFortInventoryOwnerInterface not found");
        return;
    }

    InventoryOwnerInterfaceOffset = reinterpret_cast<uint8_t*>(InventoryOwner) - reinterpret_cast<uint8_t*>(PawnDefault);
    const uint8_t* PawnRemove = static_cast<const uint8_t*>((*reinterpret_cast<void***>(InventoryOwner))[RemoveIndex]);
    // The stripped stub is `xor al, al; ret`; on an exe with moved code (1.9.1) its return is rewritten.
    if (!Finder::BodyIs(reinterpret_cast<uintptr_t>(PawnRemove), { 0x32, 0xC0, 0xC3 }))
    {
        Log::Error(L"Finder: AFortAIPawn::RemoveInventoryItem is not the stripped stub");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** InterfaceVTable = ClassDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(ClassDefault) + InventoryOwnerInterfaceOffset) : nullptr;
        if (InterfaceVTable && InterfaceVTable[RemoveIndex] == PawnRemove)
        {
            Hooks::SwapVTable(L"AFortAIPawn::RemoveInventoryItem", InterfaceVTable, RemoveIndex, reinterpret_cast<void*>(&RemoveInventoryItemHK));
        }
    }
}

bool AFortAIPawn::RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
    AFortAIPawn* Pawn = reinterpret_cast<AFortAIPawn*>(static_cast<uint8_t*>(InventoryOwner) - InventoryOwnerInterfaceOffset);
    return Pawn->RemoveInventoryItem(ItemGuid, Count, bForceRemoveFromQuickBars);
}
