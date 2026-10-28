#include "pch.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Abilities/FortHomebaseSet.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortGameStateAthena.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortKismetLibrary.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Items/FortQuickBars.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Logging/LogMacros.h"
#include "Math/UnrealMathUtility.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPawn.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

#include <algorithm>
#include <cmath>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

bool AFortPlayerControllerAthena::PlaysBattleRoyaleHeroes()
{
    return DefaultHeroes.Num() > 0 || Member_CustomizationLoadout.IsAvailable();
}

bool AFortPlayerControllerAthena::IsDefaultHero(UFortHero* Hero)
{
    UFortHeroType* HeroType = Hero ? Hero->GetHeroTypeBP() : nullptr;
    for (UFortHeroType* DefaultHero : DefaultHeroes)
    {
        if (DefaultHero && DefaultHero == HeroType)
        {
            return true;
        }
    }
    return HeroType && HeroType == GetLoadoutHero();
}

UFortHeroType* AFortPlayerControllerAthena::GetLoadoutHero()
{
    UAthenaCharacterItemDefinition* Character = Member_CustomizationLoadout.IsAvailable() ? static_cast<UAthenaCharacterItemDefinition*>(CustomizationLoadout.Character) : nullptr;
    return Character ? static_cast<UFortHeroType*>(Character->HeroDefinition) : nullptr;
}

UFortWorldItemDefinition* AFortPlayerControllerAthena::GetLoadoutPickaxe()
{
    // The loadout is the athena profile's favorite_* items, and nothing fills an empty favorite_pickaxe in: the
    // profile's loadout (0x41B610) replaces the whole loadout (0x41B2A0), and only an empty dance gets a fallback. The
    // pickaxe is then the one Athena_PlayerController_C's default loadout has.
    if (!Member_CustomizationLoadout.IsAvailable())
    {
        return nullptr;
    }
    UAthenaPickaxeItemDefinition* Pickaxe = CustomizationLoadout.Pickaxe;
    if (!Pickaxe)
    {
        const AFortPlayerControllerAthena* Default = static_cast<const AFortPlayerControllerAthena*>(GetClass()->GetDefaultObject(false));
        Pickaxe = Default ? static_cast<UAthenaPickaxeItemDefinition*>(Default->CustomizationLoadout.Pickaxe) : nullptr;
    }
    return Pickaxe ? static_cast<UFortWeaponItemDefinition*>(Pickaxe->WeaponDefinition) : nullptr;
}

UFortHero* AFortPlayerControllerAthena::GetOrInitializeHero()
{
    // The override (1.7.2 0x42E5E0) keeps the controller's hero, or makes a hero of the next of its DefaultHeroes (a
    // counter over every controller, divided by their number) and sets it through TrySetHeroImplementation. 1.8's
    // (0x4347F0) has no DefaultHeroes: it first makes the loadout from the athena profile (GetAthenaLoadoutWithOverrides,
    // 0x827300; a random character of AthenaGameData's when it has none) and then the hero of its character.
    if (!PlaysBattleRoyaleHeroes() || !Finder::ResolveAthenaHeroLive())
    {
        return nullptr;
    }
    return CallVirtual<UFortHero*>(this, Natives::AFortPlayerController_GetOrInitializeHeroIndex);
}

UFortHero* AFortPlayerControllerAthena::SetupHero()
{
    // Battle Royale played a level 1 Save the World soldier's stats (#73: Soldier_Balanced_UC_T1 in AttributesHeroScaling,
    // 440 health and 172 shield) instead of its hero's (GE_Map_Athena_Vitality_Health: 100 health and a 100 shield cap,
    // GE_Map_Athena_StartingShields: no shield). GetOrInitializeHero binds the player's UFortRegisteredPlayerInfo to the
    // player state before it makes the hero, and binding initializes the attributes (0x5EA3F0) with the controller's hero
    // or, while there is none, with UFortGameData::DefaultHero (HID_Commando_GrenadeGun_UC_T01). Setting the hero
    // afterwards doesn't initialize them again; ResetGameplayAttributes does, with AFortPlayerController::GetHero (0x808A80),
    // which only returns the hero whose id the player state holds. So with the pawn possessed the hero is set once more,
    // which puts its id back on the player state, before Possess resets the attributes with it.
    UFortHero* Hero = GetOrInitializeHero();
    if (Hero)
    {
        TrySetHeroImplementation(Hero, false, false);
    }
    return Hero;
}

void AFortPlayerControllerAthena::SetupBackpackSize()
{
    // The HUD's backpack showed "0 | 0" instead of "0 | 5" (#77). AthenaPickupManager's RefreshCounts shows
    // UFortInventoryContext::GetBackpackItemCounts (0x2DB19B0): the world inventory's limited items against its size
    // (0x63CA40), which is OverriddenBackpackSize when set, else the player info's ability system actor's (the player
    // state's) FortHomebaseSet.WorldInventorySizeBonus. Battle Royale's +5 to that is GE_Map_Athena_Inventory_StartingSize
    // in its heroes' CombinedStatGEs, which reach the player state only when Possess resets the attributes (#73, before
    // that never), after the starting items, and the HUD reads the size only when it's built and when the inventory
    // changes. The size goes on the controller instead, read from the same effects, before the starting items. It
    // replicates while the controller has a cheat manager (0x81FBA0), which every player gets when they join.
    // 1.8 showed "0 | 0" again (#106): its Athena_PlayerController_C has no DefaultHeroes. Its heroes are the loadout
    // characters' (HID_*_Athena_Commando_*, with the same effect), and the loadout is made when the pawn first spawns
    // (GetOrInitializeHero), so the size is set by Possess then, before it gives the loadout's pickaxe.
    std::vector<UFortHeroType*> Heroes(DefaultHeroes.begin(), DefaultHeroes.end());
    if (UFortHeroType* LoadoutHero = GetLoadoutHero())
    {
        Heroes.push_back(LoadoutHero);
    }
    if (Heroes.empty() && Member_CustomizationLoadout.IsAvailable())
    {
        return;
    }

    UClass* HomebaseSetClass = UFortHomebaseSet::StaticClass();
    const UProperty* WorldInventorySizeBonus = HomebaseSetClass ? HomebaseSetClass->FindPropertyByName(FName(L"WorldInventorySizeBonus", FNAME_Find)) : nullptr;
    for (UFortHeroType* HeroType : Heroes)
    {
        const int32_t BackpackSize = HeroType ? static_cast<int32_t>(HeroType->GetCombinedStatBonus(WorldInventorySizeBonus)) : 0;
        if (BackpackSize > 0)
        {
            OverriddenBackpackSize = BackpackSize;
            UE_LOG(LogFort, Log, TEXT("SetupBackpackSize: %s has a backpack of %d (%s)"), *GetName(), BackpackSize, *HeroType->GetName());
            return;
        }
    }
    UE_LOG(LogFort, Warning, TEXT("SetupBackpackSize: %s's heroes give no backpack size, it stays the attribute's"), *GetName());
}

void AFortPlayerControllerAthena::GiveLoadoutPickaxe()
{
#if WITH_SERVER_CODE
    // 1.8 players had no pickaxe (#106): 1.8's Athena_GameMode StartingItems are only the building pieces, the edit tool
    // and wood, the pickaxe being the loadout's (DefaultPickaxe: WID_Harvest_Pickaxe_Athena_C_T01), and nothing in the
    // shipping exe gives it (the controller's loadout pickaxe, +0x2900, is touched only by constructors). It goes in the
    // harvesting tool's slot, primary 0, before Possess activates the focused slot.
    UFortWorldItemDefinition* Pickaxe = Role == ROLE_Authority && WorldInventory ? GetLoadoutPickaxe() : nullptr;
    if (Pickaxe && GetItemQuantity(Pickaxe) <= 0)
    {
        UFortKismetLibrary::K2_GiveItemToPlayer(this, Pickaxe, 1, false);
        UE_LOG(LogFort, Log, TEXT("GiveLoadoutPickaxe: %s gets %s"), *GetName(), *Pickaxe->GetName());
    }
#endif
}

void AFortPlayerControllerAthena::DropInventoryOnDeath(AFortPawn* DeadPawn)
{
#if WITH_SERVER_CODE
    // An eliminated player's loot went with them (#89): the pawn-death handler's inventory drop (1.7.2 0x864F55, before
    // the death report and the elimination) is stripped. The only drop on death left is Save the World's backpack
    // (AFortPlayerController slot 0x2C0, 0x80DB20, "Removing %d items from inventory on death": one backpack pickup
    // sized by the game mode's DeathPenaltyData), which nothing calls either, and Battle Royale's Anthem_DeathPenalty
    // (respawn delays only) would leave it empty. Battle Royale drops each item as its own pickup: whole stacks of what
    // bCanBeDropped allows, so not the pickaxe or the edit tool, nor the building pieces (UFortBuildingItemDefinition's
    // constructor 0x6249D0 clears it), with the weapons' loaded ammo. bDropOnDeath is not the rule: in 1.7.2's content
    // only Athena_Shields sets it. The warmup island respawns the player with what they hold, so nothing drops until the
    // aircraft, when a death is an elimination (0x437C30 takes the player out of the alive players after Warmup).
    UWorld* World = DeadPawn && Role == ROLE_Authority ? GetWorld() : nullptr;
    AFortGameStateAthena* GameState = World ? Cast<AFortGameStateAthena>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    AFortInventory* Inventory = WorldInventory;
    if (!GameState || GameState->GamePhase <= EAthenaGamePhase::GetWarmup() || !Inventory)
    {
        return;
    }

    std::vector<UFortWorldItem*> Drops;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        UFortWorldItem* WorldItem = Instances[Index];
        FFortItemEntry* ItemEntry = WorldItem ? &static_cast<FFortItemEntry&>(WorldItem->ItemEntry) : nullptr;
        UFortWorldItemDefinition* ItemDefinition = ItemEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry->ItemDefinition)) : nullptr;
        if (ItemDefinition && ItemDefinition->bCanBeDropped && ItemEntry->Count > 0)
        {
            Drops.push_back(WorldItem);
        }
    }

    const FVector DeathLocation = DeadPawn->GetTransform().GetLocation();
    const int32_t DropCount = static_cast<int32_t>(Drops.size());
    const std::vector<FVector> Spots = GetInventoryDropSpots(DeadPawn, DeathLocation, DropCount);

    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* DropEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    int32_t Dropped = 0;
    for (int32_t Index = 0; Index < DropCount; ++Index)
    {
        FFortItemEntry& ItemEntry = Drops[Index]->ItemEntry;
        const FGuid ItemGuid = ItemEntry.ItemGuid;
        const int32_t Count = ItemEntry.Count;
        EntryStruct->InitializeStruct(DropEntry);
        EntryStruct->CopyScriptStruct(DropEntry, &ItemEntry);
        if (RemoveInventoryItem(ItemGuid, Count, false))
        {
            if (AFortPickup* Pickup = SpawnDropPickup(*DropEntry, &DeathLocation, nullptr))
            {
                Pickup->PawnWhoDroppedPickup = DeadPawn;
                Pickup->TossOntoFloor(Spots[Index], DeadPawn);
                ++Dropped;
            }
        }
        EntryStruct->DestroyStruct(DropEntry);
    }
    UE_LOG(LogFort, Log, TEXT("DropInventoryOnDeath: %s dropped %d of %d items"), *GetName(), Dropped, DropCount);
#endif
}

std::vector<FVector> AFortPlayerControllerAthena::GetInventoryDropSpots(AActor* DeadPawn, const FVector& DeathLocation, int32_t Count)
{
    // The pickups are tossed from the body to their own spots around it. A ring 80 out with 50 between them left a pile
    // (#89: pickups are 60 across, AFortPickup's constructor 0x65BA90). They scatter a few meters out instead: rings from
    // 1.5m, a meter apart, with about a meter between neighbours, each ring turned against the last and each spot moved a
    // little at random. The stripped death drop leaves nothing to say how far 1.7.2 threw them; the toss itself
    // (AFortPickup::TossPickup, 0x6622F0) only has a fallback, a pickup aimed at the spot it starts from goes 512 out in a
    // random direction (VRand, 0x4F4E10), so these distances are chosen. A wall between the body and a spot pulls the spot
    // in to the wall, and each spot is traced down to the floor below it, from a little over the body's height (the toss
    // lands a pickup where it is aimed and finds no ground, #72).
    constexpr float Pi = 3.1415926535897932f;
    constexpr float FirstRingRadius = 150.0f;
    constexpr float RingSpacing = 100.0f;
    constexpr float PickupSpacing = 110.0f;
    constexpr float PickupRadius = 30.0f;
    constexpr float TraceHeight = 50.0f;
    constexpr float TraceDepth = 100000.0f;
    std::vector<FVector> Spots;
    const float StartAngle = FMath::FRand() * 2.0f * Pi;
    for (float Radius = FirstRingRadius; static_cast<int32_t>(Spots.size()) < Count; Radius += RingSpacing)
    {
        const int32_t RingCount = std::min(std::max(static_cast<int32_t>(2.0f * Pi * Radius / PickupSpacing), 1), Count - static_cast<int32_t>(Spots.size()));
        const float Step = 2.0f * Pi / static_cast<float>(RingCount);
        const float RingStart = StartAngle + Radius / FirstRingRadius;
        for (int32_t Index = 0; Index < RingCount; ++Index)
        {
            const float Angle = RingStart + Step * (static_cast<float>(Index) + (FMath::FRand() - 0.5f) * 0.4f);
            const float Distance = Radius + (FMath::FRand() - 0.5f) * RingSpacing * 0.4f;
            const FVector Start(DeathLocation.X, DeathLocation.Y, DeathLocation.Z + TraceHeight);
            FVector Spot(Start.X + std::cos(Angle) * Distance, Start.Y + std::sin(Angle) * Distance, Start.Z);
            FVector WallHit;
            if (UKismetSystemLibrary::LineTraceSingle(DeadPawn, Start, Spot, 0, false, true, WallHit))
            {
                const float Reach = std::max(std::sqrt(FMath::Square(WallHit.X - Start.X) + FMath::Square(WallHit.Y - Start.Y)) - PickupRadius, 1.0f);
                Spot = FVector(Start.X + std::cos(Angle) * Reach, Start.Y + std::sin(Angle) * Reach, Start.Z);
            }
            FVector Floor = Spot;
            UKismetSystemLibrary::LineTraceSingle(DeadPawn, Spot, FVector(Spot.X, Spot.Y, Spot.Z - TraceDepth), 0, false, true, Floor);
            Spots.push_back(Floor);
        }
    }
    return Spots;
}

void AFortPlayerControllerAthena::HookCallSites()
{
    if (uint8_t* Call = Finder::PawnDiedInventoryDropCall())
    {
        Hooks::RedirectCall(L"AFortPlayerControllerZone pawn-death inventory drop", Call, reinterpret_cast<void*>(&PawnDiedInventoryDropCall));
    }
}

void AFortPlayerControllerAthena::PawnDiedInventoryDropCall(AFortPlayerController* Controller, void* Unused, const FGameplayTagContainer* DamageTags, AFortPawn* DeadPawn, bool* bOutDroppedBackpack)
{
    // The handler passes the controller's MyFortPawn. Battle Royale drops no backpack, so bDroppedBackpack stays false
    // and the other controllers keep the stub's nothing.
    if (AFortPlayerControllerAthena* AthenaController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(Controller)))
    {
        AthenaController->DropInventoryOnDeath(DeadPawn);
    }
}

void AFortPlayerControllerAthena::ResetToStartingItems()
{
#if WITH_SERVER_CODE
    // What the warmup island gave stayed for the whole match (#86). The aircraft phase (StartAircraftPhase, 1.7.2 0x3FFB60)
    // calls a stripped function with each controller after it destroys the warmup pawn (Finder::AircraftPhasePlayerCleanupCall;
    // on 1.8.1/1.8.2 the one just before, once the pawn's weapon is unequipped), which runs this: every item that isn't one of Athena_GameMode's StartingItems goes (weapons, ammo, consumables, traps,
    // and the materials, which start at 0), the starting items stay at their starting count.
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this);
    AFortInventory* Inventory = WorldInventory;
    if (Role != ROLE_Authority || !GameMode || !Inventory)
    {
        return;
    }

    // 1.8's pickaxe is the loadout's, not a starting item: it stays too.
    const TStructArray<FItemAndCount>& StartingItems = GameMode->StartingItems;
    const UFortWorldItemDefinition* Pickaxe = GetLoadoutPickaxe();
    const auto StartingCount = [&StartingItems, Pickaxe](const UFortItemDefinition* ItemDefinition)
    {
        if (Pickaxe && ItemDefinition == Pickaxe)
        {
            return 1;
        }
        for (int32_t Index = 0; Index < StartingItems.Num(); ++Index)
        {
            const FItemAndCount& Entry = StartingItems[Index];
            if (static_cast<UFortItemDefinition*>(Entry.Item) == ItemDefinition)
            {
                return std::max(static_cast<int32_t>(Entry.Count), 0);
            }
        }
        return 0;
    };

    // Count -1 takes the whole item.
    std::vector<std::pair<FGuid, int32_t>> Removals;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        const int32_t Keep = Entry ? StartingCount(Entry->ItemDefinition) : 0;
        if (Entry && Entry->Count > Keep)
        {
            Removals.emplace_back(Entry->ItemGuid, Keep > 0 ? Entry->Count - Keep : -1);
        }
    }

    // A focused slot that empties would be activated again (ServerRemoveItemInternal), for a pawn that is gone. The
    // player leaves the aircraft with the harvesting tool (slot 0) instead, which Possess equips.
    AFortQuickBars* Bars = QuickBars;
    FQuickBar* PrimaryQuickBar = Bars ? &static_cast<FQuickBar&>(Bars->PrimaryQuickBar) : nullptr;
    const int32_t FocusedSlot = PrimaryQuickBar ? static_cast<int32_t>(PrimaryQuickBar->CurrentFocusedSlot) : -1;
    if (FocusedSlot > 0 && FocusedSlot < PrimaryQuickBar->Slots.Num())
    {
        const TArray<FGuid>& SlotItems = Bars->GetSlot(EFortQuickBars::GetPrimary(), FocusedSlot).Items;
        const bool bEmptied = std::all_of(SlotItems.begin(), SlotItems.end(), [&Removals](const FGuid& Guid)
        {
            return std::any_of(Removals.begin(), Removals.end(), [&Guid](const auto& Removal) { return Removal.first == Guid && Removal.second < 0; });
        });
        if (bEmptied)
        {
            PrimaryQuickBar->CurrentFocusedSlot = 0;
            PrimaryQuickBar->PreviousFocusedSlot = 0;
        }
    }

    for (const auto& [ItemGuid, Count] : Removals)
    {
        RemoveInventoryItem(ItemGuid, Count, true);
    }
    GiveStartingItems(StartingItems);
    UE_LOG(LogFort, Log, TEXT("ResetToStartingItems: %s boards the aircraft with its starting items, %d warmup items taken"), *GetName(), static_cast<int32_t>(Removals.size()));
#endif
}
