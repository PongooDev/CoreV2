#include "pch.h"
#include "Building/BuildingItemCollectorActor.h"
#include "Athena/FortAthenaMapInfo.h"
#include "Athena/FortGameModeAthena.h"
#include "Engine/AssetManager.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortKismetLibrary.h"
#include "Hooks.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortLootPackage.h"
#include "Items/FortPickup.h"
#include "Log.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace
{
    // An item by its primary asset id (FItemCollectorOverrideItemRow's: "Weapon:Athena_PlaysetGrenade_Instance_BuildTheTower"),
    // through the asset manager's list of that primary asset type.
    UFortWorldItemDefinition* LoadPrimaryAssetItem(const FItemCollectorPrimaryAssetId& Id)
    {
        UAssetManager* Manager = GEngine ? static_cast<UAssetManager*>(GEngine->AssetManager) : nullptr;
        TArray<FAssetData> AssetDataList;
        if (!Manager || Id.PrimaryAssetName.IsNone() || !Manager->GetPrimaryAssetDataList(Id.PrimaryAssetType, AssetDataList))
        {
            return nullptr;
        }
        for (int32_t Index = 0; Index < AssetDataList.Num(); ++Index)
        {
            const FAssetData& AssetData = AssetDataList[Index];
            if (AssetData.AssetName == Id.PrimaryAssetName)
            {
                return LoadObject<UFortWorldItemDefinition>(nullptr, *AssetData.ObjectPath.ToString(), nullptr, static_cast<uint32_t>(LOAD_NoWarn | LOAD_Quiet));
            }
        }
        return nullptr;
    }
}

const FItemCollectorRow* ABuildingItemCollectorActor::GetItemCollectorRow() const
{
    if (!Member_ItemCollectorHandle.IsAvailable() || !FItemCollectorRow::StaticStruct())
    {
        return nullptr;
    }
    FDataTableRowHandle& Handle = ItemCollectorHandle;
    UDataTable* Table = Handle.DataTable;
    const FName RowName = Handle.RowName;
    if (!Table || RowName.IsNone() || static_cast<UScriptStruct*>(Table->RowStruct) != FItemCollectorRow::StaticStruct())
    {
        return nullptr;
    }
    return reinterpret_cast<const FItemCollectorRow*>(Table->FindRowUnchecked(RowName));
}

bool ABuildingItemCollectorActor::ApplyOverrideItems(const FItemCollectorRow& Row)
{
    // The row's items in its table's row order (Item.01, Item.02, Item.03): the first for the first input (wood), and so
    // on. Its OverrideGoal is the price, which the game's own GetDepositGoal reads from the row (6.21 0xD5D9D0).
    UDataTable* Table = Row.OverrideItemsTable;
    if (!Row.bOverrideLootRules || !Table || !FItemCollectorOverrideItemRow::StaticStruct() || static_cast<UScriptStruct*>(Table->RowStruct) != FItemCollectorOverrideItemRow::StaticStruct())
    {
        return false;
    }
    std::vector<std::pair<std::wstring, const FItemCollectorOverrideItemRow*>> Rows;
    for (auto& Pair : Table->GetRowMap())
    {
        Rows.emplace_back(Pair.Key.ToString().ToStdString(), reinterpret_cast<const FItemCollectorOverrideItemRow*>(Pair.Value));
    }
    std::sort(Rows.begin(), Rows.end(), [](const auto& Left, const auto& Right) { return Left.first < Right.first; });

    TStructArray<FCollectorUnitInfo>& Collections = ItemCollections;
    for (int32_t Index = 0; Index < Collections.Num(); ++Index)
    {
        FCollectorUnitInfo& Collection = Collections[Index];
        TStructArray<FFortItemEntry>& Output = Collection.OutputItemEntry;
        Output.Empty();
        const FItemCollectorOverrideItemRow* ItemRow = Index < static_cast<int32_t>(Rows.size()) ? Rows[Index].second : nullptr;
        UFortWorldItemDefinition* Item = ItemRow ? LoadPrimaryAssetItem(ItemRow->ItemPrimaryAssetId) : nullptr;
        const int32_t Quantity = ItemRow ? std::max<int32_t>(ItemRow->Quantity, 1) : 0;
        if (Item)
        {
            const int32_t EntryIndex = Output.Add(1, TStructArray<FFortItemEntry>::ElementSize());
            FFortItemEntry::Construct(&Output[EntryIndex], Item, Quantity, 0);
            Output[EntryIndex].LoadFullClip();
        }
        UFortWorldItemDefinition* Input = Collection.InputItem;
        Log::Info(L"Vending machine: {} sells {} x{} ({} of {}) for {}", GetName().ToStdString(), Item ? Item->GetName().ToStdString() : L"nothing", Quantity,
            ItemRow ? Rows[Index].first : std::wstring(L"no row"), Table->GetName().ToStdString(), Input ? Input->GetName().ToStdString() : L"nothing");
    }
    return true;
}

void ABuildingItemCollectorActor::FinishItemSelectionSetup()
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !HasOutputItems())
    {
        return;
    }

    // From 6.20: a collector with an ItemCollectorHandle row (the Playground's machines: ItemCollectorData's
    // ItemCollector.ChallengeGames.01 to .05, .SpikeArena_01 to _03, .PlaygroundGun...) takes its rarity
    // (bUseOverrideRarity: OverrideRarity, EFortRarity's Handmade to Fine for Common to Legendary, the one GetOverrideRarity
    // gives its screen) and, with bOverrideLootRules, sells its OverrideItemsTable's items (ItemCollectorOverrideItemRow:
    // a primary asset id and a quantity) for OverrideGoal, the challenge grenades at 0. Rolled from the loot tables,
    // these sold whatever the playlist's vending rows gave, or nothing (CoreV2 #436, #440).
    if (const FItemCollectorRow* Row = GetItemCollectorRow())
    {
        if (Row->bUseOverrideRarity)
        {
            StartingGoalLevel = std::min<int32_t>(Row->OverrideRarity, 4);
        }
        if (ApplyOverrideItems(*Row))
        {
            return;
        }
    }

    // The loot tier of the collector's level (a vending machine's Loot_AthenaVending rows: tier 0 Common to 4
    // Legendary, a weapon with its ammo or a consumable), each input's own roll. The rolls are tried again (up to 8
    // times) for an item another input already sells, so the three inputs sell three items.
    AFortGameModeAthena* GameMode = AFortGameModeAthena::GetCurrent(this);
    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    const int32_t WorldLevel = GameState ? static_cast<int32_t>(GameState->WorldLevel) : -1;
    const int32_t LootTier = StartingGoalLevel;
    std::vector<UFortItemDefinition*> Offered;
    TStructArray<FCollectorUnitInfo>& Collections = ItemCollections;
    for (int32_t Index = 0; Index < Collections.Num(); ++Index)
    {
        FCollectorUnitInfo& Collection = Collections[Index];
        if (Collection.bUseDefinedOutputItem)
        {
            continue;
        }
        // The Battle Royale game mode's RedirectLootTierGroup (3.5.1 0x9F5980) keeps only Battle Royale's own groups, among
        // them one per vending rarity (Loot_AthenaVendingCommon to Loot_AthenaVendingLegendary), and turns any other into
        // None: the machines' Loot_AthenaVending among them. The client's loot tiers (AthenaLootTierData_Client) only have
        // Loot_AthenaVending, its rows' LootTier the rarity (0 Common to 4 Legendary). So the rarity's group is used where
        // the world's tables have it, else the collector's own group at the rarity's loot tier, not redirected.
        static const wchar_t* const RarityGroups[] = { L"Loot_AthenaVendingCommon", L"Loot_AthenaVendingUncommon", L"Loot_AthenaVendingRare", L"Loot_AthenaVendingEpic", L"Loot_AthenaVendingLegendary" };
        const FName Override = Collection.OverrideOutputItemLootTierGroupName;
        const FName OwnGroup = Override.IsNone() ? static_cast<FName>(DefaultItemLootTierGroupName) : Override;
        const FName RarityGroup = GameMode && LootTier >= 0 && LootTier < 5 ? GameMode->RedirectLootTierGroup(FName(RarityGroups[LootTier])) : FName();

        TStructArray<FFortItemEntry>& Output = Collection.OutputItemEntry;
        UFortItemDefinition* Picked = nullptr;
        FName LootTierKey;
        FName TierGroup;
        const FGameplayTagContainer& ContextTags = StaticGameplayTags;
        for (int32_t Try = 0; Try < 8; ++Try)
        {
            Output.Empty();
            int32_t PickedTier = -1;
            TierGroup = RarityGroup;
            bool bPicked = !RarityGroup.IsNone() && UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, PickedTier, RarityGroup, WorldLevel, -1, ContextTags);
            if (!bPicked)
            {
                TierGroup = OwnGroup;
                bPicked = UFortKismetLibrary::PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, PickedTier, OwnGroup, WorldLevel, LootTier, ContextTags);
            }
            if (bPicked)
            {
                UFortLootPackage::PickLootDrops(Output, WorldLevel, LootTierKey, nullptr, ContextTags, false, false);
            }
            Picked = Output.Num() > 0 ? static_cast<UFortItemDefinition*>(Output[0].ItemDefinition) : nullptr;
            if (!Picked || std::find(Offered.begin(), Offered.end(), Picked) == Offered.end())
            {
                break;
            }
        }
        // Battle Royale's weapons come loaded, as the containers' do (#85).
        for (int32_t EntryIndex = 0; EntryIndex < Output.Num(); ++EntryIndex)
        {
            Output[EntryIndex].LoadFullClip();
        }
        Offered.push_back(Picked);

        UFortWorldItemDefinition* Input = Collection.InputItem;
        Log::Info(L"Vending machine: {} (rarity {}) sells {} x{} ({}, {} at world level {}) for {} {}", GetName().ToStdString(), LootTier, Picked ? Picked->GetName().ToStdString() : L"nothing",
            Output.Num() > 0 ? static_cast<int32_t>(Output[0].Count) : 0, LootTierKey.ToString().ToStdString(), TierGroup.ToString().ToStdString(), WorldLevel,
            static_cast<int32_t>(static_cast<FScalableFloat&>(Collection.InputCount).GetValueAtLevel(static_cast<float>(LootTier))), Input ? Input->GetName().ToStdString() : L"nothing");
    }
#endif
}

void ABuildingItemCollectorActor::GrantOutput(AFortPlayerController* PlayerController, UFortWorldItemDefinition* InputItem)
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    if (Role != ROLE_Authority || !PlayerController || !World || !HasOutputItems())
    {
        return;
    }

    TStructArray<FCollectorUnitInfo>& Collections = ItemCollections;
    FCollectorUnitInfo* Collection = nullptr;
    for (int32_t Index = 0; Index < Collections.Num() && !Collection; ++Index)
    {
        Collection = static_cast<UFortWorldItemDefinition*>(Collections[Index].InputItem) == InputItem ? &Collections[Index] : nullptr;
    }
    if (!Collection)
    {
        Log::Warning(L"Vending machine: {} has no input for {}, {} gets nothing", GetName().ToStdString(), GetNameSafe(InputItem).ToStdString(), PlayerController->GetName().ToStdString());
        return;
    }

    // The output leaves from LootSpawnLocation (the vending machine's (0, 50, 20): its front, local +Y) in
    // LootTossDirection, turned with the collector, within LootTossConeHalfAngle; with bTossOnGround (its
    // Default.VendingMachine.DropLootOnGroundBool row) it lands on the floor in front of it instead of flying to the player.
    const FTransform Transform = GetTransform();
    const FVector LocalSpawn = LootSpawnLocation;
    const FVector SpawnLocation = Transform.TransformPosition(LocalSpawn);
    constexpr float DegreesToRadians = 3.1415926535897932f / 180.0f;
    const FRotator TossRotation = LootTossDirection;
    FVector LocalDirection(std::cos(TossRotation.Yaw * DegreesToRadians), std::sin(TossRotation.Yaw * DegreesToRadians), 0.0f);
    if (TossRotation.Yaw == 0.0f && TossRotation.Pitch == 0.0f && (LocalSpawn.X != 0.0f || LocalSpawn.Y != 0.0f))
    {
        const float Length = std::sqrt(LocalSpawn.X * LocalSpawn.X + LocalSpawn.Y * LocalSpawn.Y);
        LocalDirection = FVector(LocalSpawn.X / Length, LocalSpawn.Y / Length, 0.0f);
    }
    const float ConeHalfAngle = std::clamp(static_cast<float>(LootTossConeHalfAngle), 0.0f, 90.0f) * DegreesToRadians;
    AFortPawn* Pawn = PlayerController->MyFortPawn;

    std::vector<uint8_t> DefinedStorage;
    std::vector<FFortItemEntry*> Entries;
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    if (Collection->bUseDefinedOutputItem && static_cast<UFortWorldItemDefinition*>(Collection->OutputItem))
    {
        DefinedStorage.assign(EntryStruct->GetStructureSize(), 0);
        FFortItemEntry* Entry = reinterpret_cast<FFortItemEntry*>(DefinedStorage.data());
        FFortItemEntry::Construct(Entry, Collection->OutputItem, 1, 0);
        Entry->LoadFullClip();
        Entries.push_back(Entry);
    }
    else
    {
        TStructArray<FFortItemEntry>& Output = Collection->OutputItemEntry;
        for (int32_t Index = 0; Index < Output.Num(); ++Index)
        {
            Entries.push_back(&Output[Index]);
        }
    }

    int32_t Spawned = 0;
    const FRotator SpawnRotation(0.0f, 0.0f, 0.0f);
    for (FFortItemEntry* Entry : Entries)
    {
        AFortPickup* Pickup = AFortPickup::CreatePickup(World, *Entry, &SpawnLocation, &SpawnRotation);
        if (!Pickup)
        {
            continue;
        }
        const float Yaw = (FMath::FRand() * 2.0f - 1.0f) * ConeHalfAngle;
        const FVector Direction(LocalDirection.X * std::cos(Yaw) - LocalDirection.Y * std::sin(Yaw), LocalDirection.X * std::sin(Yaw) + LocalDirection.Y * std::cos(Yaw), 0.0f);
        const FVector Floor = Transform.TransformPosition(FVector(Direction.X * (std::abs(LocalSpawn.Y) + 100.0f), Direction.Y * (std::abs(LocalSpawn.Y) + 100.0f), 0.0f));
        if (bTossOnGround)
        {
            Pickup->TossOntoFloor(Floor, Pawn);
        }
        else if (Pawn)
        {
            Pickup->SetPickupTarget(Pawn, -1.0f, FVector(0.0f, 0.0f, 1.0f), true);
        }
        else
        {
            Pickup->TossPickup(Floor, nullptr, 0);
        }
        ++Spawned;
    }
    if (!DefinedStorage.empty())
    {
        EntryStruct->DestroyStruct(DefinedStorage.data());
    }
    Log::Info(L"Vending machine: {} gives {} {} item(s) for {}", GetName().ToStdString(), PlayerController->GetName().ToStdString(), Spawned, GetNameSafe(InputItem).ToStdString());
#endif
}

namespace
{
    // The lambda CheckDepositGoalReached binds into the output timer (3.5.1's delegate instance, after its vtable): weak
    // pointers to the collector, the controller that reached the goal and the input item (21.40: __this, WeakPC,
    // WeakItem).
    struct FGrantOutputDelegateInstance
    {
        void* VTable;
        FWeakObjectPtr Collector;
        FWeakObjectPtr PlayerController;
        FWeakObjectPtr Item;
    };

    void ExecuteGrantOutput(void* DelegateInstance)
    {
        FGrantOutputDelegateInstance* Instance = static_cast<FGrantOutputDelegateInstance*>(DelegateInstance);
        ABuildingItemCollectorActor* Collector = Cast<ABuildingItemCollectorActor>(Instance->Collector.Get());
        AFortPlayerController* PlayerController = Cast<AFortPlayerController>(Instance->PlayerController.Get());
        UFortWorldItemDefinition* Item = Cast<UFortWorldItemDefinition>(Instance->Item.Get());
        if (Collector && PlayerController)
        {
            Collector->GrantOutput(PlayerController, Item);
        }
    }
}

void ABuildingItemCollectorActor::HookVTables()
{
#if WITH_SERVER_CODE
    // From 3.5 (Battle Royale's vending machines): builds without the vending machine class keep their collectors as
    // they are.
    UClass* Class = StaticClass();
    UObject* CollectorDefault = Class ? Class->GetDefaultObject(false) : nullptr;
    UClass* InteractInterface = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInteractInterface");
    if (!CollectorDefault || !HasOutputItems() || !FindObject<UClass>(nullptr, L"/Game/Athena/Items/Gameplay/VendingMachine/B_Athena_VendingMachine.B_Athena_VendingMachine_C"))
    {
        return;
    }
    if (!Finder::ResolveItemCollectorGrantOutputLive(CollectorDefault, InteractInterface))
    {
        Log::Error(L"Vending machine: the output timer wasn't found, so nobody gets what they pay for");
        return;
    }
    void** VTable = Natives::ABuildingItemCollectorActor_GrantOutputDelegateVTable;
    const int32_t Index = Natives::ABuildingItemCollectorActor_GrantOutputDelegateExecuteIndex;
    Hooks::SwapVTable(L"ABuildingItemCollectorActor output timer Execute", VTable, Index, reinterpret_cast<void*>(&GrantOutputDelegateExecuteHK));
    Hooks::SwapVTable(L"ABuildingItemCollectorActor output timer ExecuteIfSafe", VTable, Index + 1, reinterpret_cast<void*>(&GrantOutputDelegateExecuteIfSafeHK));

    // From 6.20 (ItemCollectorHandle; the Playground's playsets bring vending machines with their streamed levels, the
    // Spiky Stadium's among them): the machines that begin play after the match's machines were set up
    // (AFortAthenaMapInfo::SpawnVendingMachines, at the match's playlist) were never set up, so they sold nothing
    // (CoreV2 #440). The game's own moment is FinishItemSelectionSetup, which BeginPlay calls at once when the playlist's
    // tables are applied (else binds it to their delegate, the machines SpawnVendingMachines then does): the builds keep
    // its client half (the first active input) and strip the server's selection of what each input sells.
    if (!Member_ItemCollectorHandle.IsAvailable() || !Member_ItemCollectorBehavior.IsAvailable())
    {
        return;
    }
    // ItemSelectionHandle (21.40's name, unreflected): the FDelegateHandle that leads the collector's members, 0x20 before
    // ItemCollectorBehavior (6.21 +0x740).
    uint8_t* SetupCall = Finder::ResolveItemCollectorSelectionSetupLive(*reinterpret_cast<void***>(CollectorDefault), Member_ItemCollectorBehavior.GetOffsetChecked() - 0x20);
    if (SetupCall)
    {
        Hooks::RedirectCall(L"ABuildingItemCollectorActor::BeginPlay -> FinishItemSelectionSetup", SetupCall, reinterpret_cast<void*>(&FinishItemSelectionSetupHK));
    }
#endif
}

void ABuildingItemCollectorActor::FinishItemSelectionSetupHK(ABuildingItemCollectorActor* This)
{
    Natives::ABuildingItemCollectorActor_FinishItemSelectionSetup(reinterpret_cast<UObject*>(This));
#if WITH_SERVER_CODE
    AFortAthenaMapInfo::SetUpLateVendingMachine(This);
#endif
}

void ABuildingItemCollectorActor::GrantOutputDelegateExecuteHK(void* DelegateInstance)
{
    ExecuteGrantOutput(DelegateInstance);
}

bool ABuildingItemCollectorActor::GrantOutputDelegateExecuteIfSafeHK(void* DelegateInstance)
{
    ExecuteGrantOutput(DelegateInstance);
    return true;
}
