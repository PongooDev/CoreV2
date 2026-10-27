#include "pch.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"
#include "Abilities/FortAbilityKit.h"
#include "Athena/FortGameModeAthena.h"
#include "Athena/FortPlayerControllerAthena.h"
#include "Abilities/FortPlayerAttrSet.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemComponent.h"
#include "Engine/World.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortGameState.h"
#include "FortWorldManager.h"
#include "GameThread.h"
#include "Heroes/FortHero.h"
#include "Heroes/FortHeroType.h"
#include "Homebase/FortHomebaseManager.h"
#include "Hooks.h"
#include "FortKismetLibrary.h"
#include "FortSpottableActorInterface.h"
#include "FortTeamInfo.h"
#include "Items/FortInventory.h"
#include "Items/FortItemDefinition.h"
#include "Items/FortPickup.h"
#include "Items/FortQuickBars.h"
#include "Finder.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Online/FortMcpProfileAccount.h"
#include "Pawns/FortPawn.h"
#include "Pawns/FortPlayerPawn.h"
#include "Player/FortPlayerState.h"
#include "Quests/FortQuestManager.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/AssetPtr.h"
#include "UObject/UnrealType.h"
#include "Weapons/FortWeapon.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

namespace
{
    // UFortAbilityKit::Gadgets holds object pointers on 1.1 and 1.2 and TAssetPtrs (0x20-byte entries) on 1.7.2.
    std::vector<UFortGadgetItemDefinition*> KitGadgets(UFortAbilityKit* Kit)
    {
        static const int32_t EntrySize = []
        {
            UClass* KitClass = UFortAbilityKit::StaticClass();
            const UProperty* Property = KitClass ? KitClass->FindPropertyByName(FName(L"Gadgets", FNAME_Find)) : nullptr;
            const UProperty* Inner = Property && Property->GetClass()->HasAnyCastFlag(CASTCLASS_UArrayProperty) ? static_cast<const UArrayProperty*>(Property)->Inner : nullptr;
            return Inner ? static_cast<int32_t>(Inner->ElementSize) : static_cast<int32_t>(sizeof(void*));
        }();

        std::vector<UFortGadgetItemDefinition*> Gadgets;
        const TArray<UFortGadgetItemDefinition*>& Entries = Kit->Gadgets;
        for (int32_t Index = 0; Index < Entries.Num(); ++Index)
        {
            const uintptr_t Entry = reinterpret_cast<uintptr_t>(Entries.GetData()) + static_cast<uintptr_t>(Index) * EntrySize;
            Gadgets.push_back(EntrySize == sizeof(FAssetPtr)
                ? reinterpret_cast<const TAssetPtr<UFortGadgetItemDefinition>*>(Entry)->LoadSynchronous()
                : *reinterpret_cast<UFortGadgetItemDefinition* const*>(Entry));
        }
        return Gadgets;
    }

    struct FHeroAbilityKitItems
    {
        TWeakObjectPtr<AFortPlayerController> PlayerController;
        std::vector<TWeakObjectPtr<UFortItemDefinition>> GrantedDefinitions;
        std::vector<FGuid> PendingQuickBarItems;
        std::vector<TWeakObjectPtr<UFortItemDefinition>> GrantedHomebaseGadgets;
    };

    std::unordered_map<const AFortPlayerController*, FHeroAbilityKitItems> HeroAbilityKitItems;

    FHeroAbilityKitItems& GetHeroAbilityKitItems(AFortPlayerController* PlayerController)
    {
        FHeroAbilityKitItems& Items = HeroAbilityKitItems[PlayerController];
        if (Items.PlayerController.Get() != PlayerController)
        {
            std::erase_if(HeroAbilityKitItems, [PlayerController](const auto& Entry) { return Entry.first != PlayerController && !Entry.second.PlayerController.Get(); });
            Items = FHeroAbilityKitItems{ TWeakObjectPtr<AFortPlayerController>(PlayerController) };
        }
        return Items;
    }

    int32_t AbilityKitItemCount(const FAbilityKitItem& KitItem, int32_t Owned)
    {
        const int32_t Quantity = KitItem.Quantity;
        const EFortReplenishmentType Replenishment = KitItem.Replenishment;
        if (Replenishment == EFortReplenishmentType::GetClampMin())
        {
            return Quantity - Owned;
        }
        if (Replenishment == EFortReplenishmentType::GetAdd())
        {
            return Quantity;
        }
        return Owned > 0 ? 0 : Quantity;
    }

    std::vector<UFortWorldItem*> AddHeroAbilityKitItem(AFortPlayerController* PlayerController, AFortInventory* Inventory, UFortWorldItemDefinition* ItemDefinition, int32_t Count, int32_t Level)
    {
        std::vector<UFortWorldItem*> Added;
        const int32_t MaxStackSize = std::max(static_cast<int32_t>(ItemDefinition->MaxStackSize), 1);
        int32_t Remaining = Count;

        FFortItemList& ItemList = Inventory->Inventory;
        TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
        for (int32_t Index = 0; Index < Instances.Num() && Remaining > 0; ++Index)
        {
            UFortWorldItem* Existing = Instances[Index];
            FFortItemEntry* ExistingEntry = Existing ? &static_cast<FFortItemEntry&>(Existing->ItemEntry) : nullptr;
            if (ExistingEntry && static_cast<UFortItemDefinition*>(ExistingEntry->ItemDefinition) == ItemDefinition && ExistingEntry->Count < MaxStackSize)
            {
                const int32_t Merged = std::min(Remaining, MaxStackSize - ExistingEntry->Count);
                Inventory->UpdateWorldItemCount(Existing, ExistingEntry->Count + Merged);
                Remaining -= Merged;
            }
        }

        while (Remaining > 0)
        {
            const int32_t StackCount = std::min(Remaining, MaxStackSize);
            UFortWorldItem* WorldItem = Cast<UFortWorldItem>(ItemDefinition->CreateTemporaryItemInstanceBP(StackCount, Level));
            if (!WorldItem)
            {
                break;
            }

            WorldItem->SetOwningControllerForTemporaryItem(PlayerController);
            Inventory->AddWorldItem(WorldItem);
            Added.push_back(WorldItem);
            Remaining -= StackCount;
        }
        return Added;
    }
}

bool AFortPlayerController::InitializeMcpProfile(const void* OnComplete)
{
    const bool bInitialized = InitializeMcpProfileOG(this, OnComplete);

#if WITH_SERVER_CODE
    // Cheats are on for every player from the moment they join (#65, #69), pawn or not: Possess grants it too, but a
    // player without a pawn (Battle Royale before its spawn was restored) never got one, and every "cheat" was dropped.
    if (Role == ROLE_Authority && !CheatManager)
    {
        AddCheats(true);
        UE_LOG(LogFort, Log, TEXT("InitializeMcpProfile: %s has cheat manager %s"), *GetNameSafe(this), *GetNameSafe(static_cast<UObject*>(CheatManager)));
    }

    if (Role == ROLE_Authority && !QuickBars)
    {
        SpawnQuickBars();
        UWorld* World = GetWorld();
        AFortGameModeAthena* AthenaGameMode = World ? Cast<AFortGameModeAthena>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
        if (AthenaGameMode)
        {
            if (AFortPlayerControllerAthena* AthenaController = Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this)))
            {
                AthenaController->SetupBackpackSize();
            }
            GiveStartingItems(AthenaGameMode->StartingItems);
        }
        else if (UFortGameData* GameData = UFortAssetManager::Get().GetGameData())
        {
            InitializeInventory(GameData->DefaultInventoryList);
        }
    }
#endif

    return bInitialized;
}

void AFortPlayerController::SetRotatePawnToCamera(bool bNewRotatePawnToCamera)
{
    Reflection::FParams Params(StaticFunction_SetRotatePawnToCamera());
    Params.Set(L"bNewRotatePawnToCamera", bNewRotatePawnToCamera);
    Params.Call(this);
}

bool AFortPlayerController::GetRotatePawnToCamera()
{
    Reflection::FParams Params(StaticFunction_GetRotatePawnToCamera());
    return Params.Call(this) ? Params.Take<bool>(L"ReturnValue") : true;
}

void AFortPlayerController::Possess(APawn* InPawn)
{
    PossessOG(this, InPawn);

#if WITH_SERVER_CODE
    // UpdateRotation only turns the pawn to the camera (FaceRotation) while this is on. The DBNO death montage's
    // AnimNotifyState_DisablePawnRotation turns it off and can't turn it back on once the dying pawn has lost its
    // controller; only ClientRestart_Implementation re-arms it, and that runs on the owning client. Without this the
    // respawned pawn keeps its spawn facing on the server, so moving any other way is backpedalling there (#64).
    if (Role == ROLE_Authority && MyFortPawn && !GetRotatePawnToCamera())
    {
        SetRotatePawnToCamera(true);
        UE_LOG(LogFort, Log, TEXT("Possess: %s had pawn-to-camera rotation off on the server, turned it back on"), *GetNameSafe(this));
    }

    // Cheats are on for every player (#65): ServerCheat_Implementation runs the client's "cheat <command>" on this
    // controller's cheat manager, which the game mode never allows on a dedicated server.
    if (Role == ROLE_Authority && !CheatManager)
    {
        AddCheats(true);
        UE_LOG(LogFort, Log, TEXT("Possess: %s has cheat manager %s"), *GetNameSafe(this), *GetNameSafe(static_cast<UObject*>(CheatManager)));
    }

    AFortPlayerState* FortPlayerState = Role == ROLE_Authority && MyFortPawn ? Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState))) : nullptr;
    if (FortPlayerState && !FortPlayerState->bHasStartedPlaying)
    {
        FortPlayerState->bHasStartedPlaying = true;
        FortPlayerState->ForceNetUpdate();
    }

    AFortPlayerControllerAthena* AthenaController = Role == ROLE_Authority && MyFortPawn && AFortGameModeAthena::GetCurrent(this) ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this)) : nullptr;
    UFortHero* AthenaHero = AthenaController ? AthenaController->SetupHero() : nullptr;

    if (AFortPlayerPawn* PlayerPawn = Role == ROLE_Authority ? Cast<AFortPlayerPawn>(reinterpret_cast<UObject*>(static_cast<AFortPawn*>(MyFortPawn))) : nullptr)
    {
        PlayerPawn->ResetGameplayAttributes();
        if (AthenaController)
        {
            UFortHeroType* HeroType = AthenaHero ? AthenaHero->GetHeroTypeBP() : nullptr;
            UE_LOG(LogFort, Log, TEXT("Possess: %s plays %s with health %.0f/%.0f, shield %.0f/%.0f"), *GetNameSafe(this), *GetNameSafe(HeroType),
                PlayerPawn->GetHealth(), PlayerPawn->GetMaxHealth(), PlayerPawn->GetShield(), PlayerPawn->GetMaxShield());
        }
    }

    AFortQuickBars* Bars = QuickBars;
    if (Role == ROLE_Authority && Bars && MyFortPawn)
    {
        // Battle Royale's inventory is the game mode's StartingItems (with its own pickaxe) and what the player loots: no
        // hero gadgets, harvesting tool from the account's attribute or homebase gadgets. The native homebase effects skip
        // Athena too (UFortRegisteredPlayerInfo 0x5DC040, through the Athena check 0x5EAF90).
        if (!AFortGameModeAthena::GetCurrent(this))
        {
            AddHeroAbilityKitGadgetsToQuickBars();
            ApplyHarvestingTool();
            ApplyHomebaseGadgets();
        }

        FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
        Bars->ServerActivateSlotInternal(EFortQuickBars::GetPrimary(), PrimaryQuickBar.CurrentFocusedSlot, 0.0f, false);
    }
#endif
}

void AFortPlayerController::SpawnQuickBars()
{
#if WITH_SERVER_CODE
    FActorSpawnParameters SpawnInfo;
    SpawnInfo.Owner = this;
    QuickBars = GetWorld()->SpawnActor<AFortQuickBars>(AFortQuickBars::StaticClass(), SpawnInfo);
    SetupQuickBars();
#endif
}

void AFortPlayerController::SetupQuickBars()
{
#if WITH_SERVER_CODE
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UWorld* World = GetWorld();
    AFortGameMode* GameMode = World ? Cast<AFortGameMode>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    if (!QuickBars)
    {
        return;
    }

    if (GameMode && GameMode->OverridesQuickBars())
    {
        QuickBars->SetupQuickbar(EFortQuickBars::GetPrimary(), GameMode->GetQuickBarDefinition(EFortQuickBars::GetPrimary()));
        QuickBars->SetupQuickbar(EFortQuickBars::GetSecondary(), GameMode->GetQuickBarDefinition(EFortQuickBars::GetSecondary()));
    }
    else if (GameData)
    {
        QuickBars->SetupQuickbar(EFortQuickBars::GetPrimary(), GameData->GetQuickBarDefinition(EFortQuickBars::GetPrimary()));
        QuickBars->SetupQuickbar(EFortQuickBars::GetSecondary(), GameData->GetQuickBarDefinition(EFortQuickBars::GetSecondary()));
    }
#endif
}

void AFortPlayerController::InitializeInventory(const TStructArray<FItemDefinitionAndCount>& Items)
{
#if WITH_SERVER_CODE
    if (!WorldInventory)
    {
        return;
    }

    UWorld* World = GetWorld();
    AFortGameState* GameState = World ? Cast<AFortGameState>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (AFortWorldManager* WorldManager = GameState ? static_cast<AFortWorldManager*>(GameState->WorldManager) : nullptr)
    {
        WorldManager->RestorePlayerQuickBars(this);
    }

    AFortInventory* Inventory = WorldInventory;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        TryAddToQuickBar(Instances[Index]);
    }

    for (int32_t Index = 0; Index < Items.Num(); ++Index)
    {
        const FItemDefinitionAndCount& Entry = Items[Index];
        const TAssetPtr<UFortItemDefinition>& EntryDefinition = Entry.ItemDefinition;
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(EntryDefinition.LoadSynchronous());
        if (ItemDefinition && ItemDefinition->ItemType == EFortItemType::GetWeaponHarvest())
        {
            continue;
        }
        if (ItemDefinition && GetItemQuantity(ItemDefinition) <= 0)
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(this, ItemDefinition, Entry.Count, false);
        }
    }
#endif
}

void AFortPlayerController::GiveStartingItems(const TStructArray<FItemAndCount>& Items)
{
#if WITH_SERVER_CODE
    for (int32_t Index = 0; WorldInventory && Index < Items.Num(); ++Index)
    {
        const FItemAndCount& Entry = Items[Index];
        UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Entry.Item));
        if (ItemDefinition && Entry.Count > 0 && GetItemQuantity(ItemDefinition) <= 0)
        {
            UFortKismetLibrary::K2_GiveItemToPlayer(this, ItemDefinition, Entry.Count, false);
        }
    }
#endif
}

void AFortPlayerController::TryAddToQuickBar(UFortWorldItem* Item)
{
#if WITH_SERVER_CODE
    if (!QuickBars || !Item)
    {
        UE_LOG(LogFort, Warning, TEXT("Failed TryAddToQuickBar due to invalid QuickBars '%s' or Item '%s'"), *GetNameSafe(static_cast<AFortQuickBars*>(QuickBars)), *GetNameSafe(Item));
        return;
    }

    FFortItemEntry& ItemEntry = Item->ItemEntry;
    UFortItemDefinition* ItemDefinition = ItemEntry.ItemDefinition;
    if (!ItemDefinition)
    {
        UE_LOG(LogFort, Warning, TEXT("Failed TryAddToQuickBar due to invalid ItemDefinition for Item '%s'"), *GetNameSafe(Item));
        return;
    }

    const FGuid ItemGuid = ItemEntry.ItemGuid;
    AFortQuickBars* Bars = QuickBars;
    if (Bars->ContainsItem(ItemGuid))
    {
        UE_LOG(LogFort, Log, TEXT("TryAddToQuickBar did not add item '%s' with definition '%s' because it is already on the quickbar"), *GetNameSafe(Item), *GetNameSafe(ItemDefinition));
        return;
    }

    const EFortItemType ItemType = ItemDefinition->ItemType;
    for (EFortQuickBars Bar : { EFortQuickBars::GetPrimary(), EFortQuickBars::GetSecondary() })
    {
        const int32_t NumSlots = Bars->GetQuickBar(Bar).Slots.Num();
        for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
        {
            const TArray<EFortItemType>& AcceptedItemTypes = Bars->GetSlotData(Bar, SlotIndex).AcceptedItemTypes;
            if (AcceptedItemTypes.Contains(ItemType) && Bars->GetSlot(Bar, SlotIndex).Items.Num() == 0)
            {
                Bars->ServerAddItemInternal(ItemGuid, Bar, SlotIndex);
                return;
            }
        }
    }
#endif
}

void AFortPlayerController::TrySetHeroImplementation(UFortHero* NewHero, bool bSendToServer, bool bUnused)
{
#if WITH_SERVER_CODE
    // Battle Royale plays one of the controller's DefaultHeroes (HID_Commando_Athena_*), which its GetOrInitializeHero
    // (1.7.2 0x42E5E0, called by the game mode when the player joins, 0x587C70) makes: their GE_Map_Athena_Vitality_Health
    // overrides MaxHealth and the shield cap to 100 and GE_Map_Athena_StartingShields the shield to 0. The account's Save
    // the World hero, which the MCP profile (0x5CAB40) and ServerSetHero (0x827E30) also set here, would bring its own
    // stats into the match instead. Possess sets the hero again (AFortPlayerControllerAthena::SetupHero).
    AFortGameModeAthena* AthenaGameMode = Role == ROLE_Authority ? AFortGameModeAthena::GetCurrent(this) : nullptr;
    AFortPlayerControllerAthena* AthenaController = AthenaGameMode && NewHero ? Cast<AFortPlayerControllerAthena>(reinterpret_cast<UObject*>(this)) : nullptr;
    if (AthenaController && AthenaController->DefaultHeroes.Num() > 0 && !AthenaController->IsDefaultHero(NewHero))
    {
        UE_LOG(LogFort, Log, TEXT("TrySetHeroImplementation: %s plays a Battle Royale hero, not %s"), *GetName(), *NewHero->GetName());
        AthenaController->GetOrInitializeHero();
        return;
    }
#endif

    TrySetHeroImplementationOG(this, NewHero, bSendToServer, bUnused);

#if WITH_SERVER_CODE
    // Hero ability kit items are Save the World gadgets and kit items.
    if (!AthenaGameMode)
    {
        ApplyHeroAbilityKitItems(NewHero);
    }
#endif
}

void AFortPlayerController::ApplyHeroAbilityKitItems(UFortHero* Hero)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(static_cast<APlayerState*>(PlayerState));
    if (Role != ROLE_Authority || !Inventory || !Hero || !FortPlayerState)
    {
        return;
    }

    std::vector<UFortAbilityKit*> Kits;
    const auto AddKit = [&Kits](UFortAbilityKit* Kit)
    {
        if (Kit && std::find(Kits.begin(), Kits.end(), Kit) == Kits.end())
        {
            Kits.push_back(Kit);
        }
    };

    UAbilitySystemComponent* AbilitySystemComponent = FortPlayerState->AbilitySystemComponent;
    if (UFortHeroType* HeroType = Hero->GetHeroTypeBP(); HeroType && AbilitySystemComponent)
    {
        TStructArray<FHeroAbilityKit>& HeroTypeKits = HeroType->AbilityKits;
        for (int32_t Index = 0; Index < HeroTypeKits.Num(); ++Index)
        {
            FHeroAbilityKit& HeroTypeKit = HeroTypeKits[Index];
            if (AbilitySystemComponent->HasAllMatchingGameplayTags(HeroTypeKit.RequiredGPTags))
            {
                const TAssetPtr<UFortAbilityKit>& InherentAbilityKit = HeroTypeKit.InherentAbilityKit;
                AddKit(InherentAbilityKit.LoadSynchronous());
            }
        }
    }
    for (UFortAbilityKit* Kit : Hero->SpecializationAbilityKits)
    {
        AddKit(Kit);
    }

    std::vector<UFortItemDefinition*> Definitions;
    std::vector<UFortWorldItemDefinition*> Gadgets;
    std::vector<const FAbilityKitItem*> KitItems;
    const auto AddDefinition = [&Definitions](UFortItemDefinition* Definition)
    {
        if (!Definition || std::find(Definitions.begin(), Definitions.end(), Definition) != Definitions.end())
        {
            return false;
        }
        Definitions.push_back(Definition);
        return true;
    };
    for (UFortAbilityKit* Kit : Kits)
    {
        for (UFortGadgetItemDefinition* Gadget : KitGadgets(Kit))
        {
            if (AddDefinition(Gadget))
            {
                Gadgets.push_back(Gadget);
            }
        }

        TStructArray<FAbilityKitItem>& Items = Kit->Items;
        for (int32_t Index = 0; Index < Items.Num(); ++Index)
        {
            const FAbilityKitItem& KitItem = Items[Index];
            UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(KitItem.Item));
            if (ItemDefinition && ItemDefinition->ItemType != EFortItemType::GetWeaponHarvest() && AddDefinition(ItemDefinition))
            {
                KitItems.push_back(&KitItem);
            }
        }
    }

    FHeroAbilityKitItems& Granted = GetHeroAbilityKitItems(this);
    std::vector<UFortItemDefinition*> PreviousDefinitions;
    for (const TWeakObjectPtr<UFortItemDefinition>& GrantedDefinition : Granted.GrantedDefinitions)
    {
        if (UFortItemDefinition* Definition = GrantedDefinition.Get())
        {
            PreviousDefinitions.push_back(Definition);
        }
    }

    std::vector<FGuid> StaleItems;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        UFortItemDefinition* Definition = Entry ? static_cast<UFortItemDefinition*>(Entry->ItemDefinition) : nullptr;
        if (Definition && std::find(PreviousDefinitions.begin(), PreviousDefinitions.end(), Definition) != PreviousDefinitions.end() && std::find(Definitions.begin(), Definitions.end(), Definition) == Definitions.end())
        {
            StaleItems.push_back(Entry->ItemGuid);
        }
    }
    for (const FGuid& ItemGuid : StaleItems)
    {
        RemoveInventoryItem(ItemGuid, -1, true);
    }

    const auto WasGranted = [&PreviousDefinitions](UFortItemDefinition* Definition)
    {
        return std::find(PreviousDefinitions.begin(), PreviousDefinitions.end(), Definition) != PreviousDefinitions.end();
    };

    const int32_t Level = std::max(Hero->GetLevel(), 1);
    for (UFortWorldItemDefinition* Gadget : Gadgets)
    {
        if (WasGranted(Gadget) || GetItemQuantity(Gadget) > 0)
        {
            continue;
        }
        for (UFortWorldItem* GadgetItem : AddHeroAbilityKitItem(this, Inventory, Gadget, 1, Level))
        {
            FFortItemEntry& GadgetEntry = GadgetItem->ItemEntry;
            Granted.PendingQuickBarItems.push_back(GadgetEntry.ItemGuid);
        }
    }

    for (const FAbilityKitItem* KitItem : KitItems)
    {
        UFortWorldItemDefinition* ItemDefinition = static_cast<UFortWorldItemDefinition*>(static_cast<UFortItemDefinition*>(KitItem->Item));
        if (WasGranted(ItemDefinition))
        {
            continue;
        }

        const int32_t Count = AbilityKitItemCount(*KitItem, GetItemQuantity(ItemDefinition));
        if (Count > 0)
        {
            AddHeroAbilityKitItem(this, Inventory, ItemDefinition, Count, Level);
        }
    }

    Granted.GrantedDefinitions.clear();
    for (UFortItemDefinition* Definition : Definitions)
    {
        Granted.GrantedDefinitions.emplace_back(Definition);
    }

    if (QuickBars && GetPawn())
    {
        AddHeroAbilityKitGadgetsToQuickBars();
    }
#endif
}

void AFortPlayerController::ApplyHarvestingTool()
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortQuickBars* Bars = QuickBars;
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    UAbilitySystemComponent* AbilitySystemComponent = FortPlayerState ? static_cast<UAbilitySystemComponent*>(FortPlayerState->AbilitySystemComponent) : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (Role != ROLE_Authority || !Inventory || !Bars || !GameData)
    {
        return;
    }

    TArray<TAssetPtr<UFortWorldItemDefinition>>& HarvestingTools = GameData->HarvestingTools;
    if (HarvestingTools.Num() == 0)
    {
        return;
    }

    int32_t Level = 0;
    if (const UFortPlayerAttrSet* PlayerAttrSet = AbilitySystemComponent ? AbilitySystemComponent->GetSet<UFortPlayerAttrSet>() : nullptr)
    {
        const FFortGameplayAttributeData& HarvestingToolAttribute = PlayerAttrSet->HarvestingTool;
        Level = std::clamp(static_cast<int32_t>(HarvestingToolAttribute.GetCurrentValue()), 0, HarvestingTools.Num() - 1);
    }
    UFortWorldItemDefinition* HarvestingTool = HarvestingTools[Level].LoadSynchronous();
    if (!HarvestingTool)
    {
        UE_LOG(LogFort, Warning, TEXT("ApplyHarvestingTool: no harvesting tool for level %d"), Level);
        return;
    }

    bool bHasHarvestingTool = false;
    std::vector<FGuid> ReplacedItems;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry* Entry = Instances[Index] ? &static_cast<FFortItemEntry&>(Instances[Index]->ItemEntry) : nullptr;
        UFortItemDefinition* Definition = Entry ? static_cast<UFortItemDefinition*>(Entry->ItemDefinition) : nullptr;
        if (!Definition || Definition->ItemType != EFortItemType::GetWeaponHarvest())
        {
            continue;
        }

        if (Definition == HarvestingTool)
        {
            bHasHarvestingTool = true;
        }
        else
        {
            ReplacedItems.push_back(Entry->ItemGuid);
        }
    }
    if (ReplacedItems.empty() && bHasHarvestingTool)
    {
        return;
    }

    int32_t QuickBarSlot = -1;
    FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
    TStructArray<FQuickBarSlot>& Slots = PrimaryQuickBar.Slots;
    for (int32_t SlotIndex = 0; SlotIndex < Slots.Num() && QuickBarSlot < 0 && !ReplacedItems.empty(); ++SlotIndex)
    {
        TArray<FGuid>& SlotItems = Slots[SlotIndex].Items;
        for (const FGuid& SlotItem : SlotItems)
        {
            if (SlotItem == ReplacedItems.front())
            {
                QuickBarSlot = SlotIndex;
                break;
            }
        }
    }

    for (const FGuid& ItemGuid : ReplacedItems)
    {
        RemoveInventoryItem(ItemGuid, -1, true);
    }

    if (bHasHarvestingTool)
    {
        return;
    }

    UFortWorldItem* WorldItem = Cast<UFortWorldItem>(HarvestingTool->CreateTemporaryItemInstanceBP(1, 1));
    if (!WorldItem)
    {
        return;
    }

    WorldItem->SetOwningControllerForTemporaryItem(this);
    Inventory->AddWorldItem(WorldItem);
    FFortItemEntry& WorldItemEntry = WorldItem->ItemEntry;
    if (QuickBarSlot >= 0)
    {
        Bars->ServerAddItemInternal(WorldItemEntry.ItemGuid, EFortQuickBars::GetPrimary(), QuickBarSlot);
    }
    else
    {
        TryAddToQuickBar(WorldItem);
    }
#endif
}

void AFortPlayerController::ApplyHomebaseGadgets()
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    AFortQuickBars* Bars = QuickBars;
    UFortRegisteredPlayerInfo* PlayerInfo = MyPlayerInfo;
    UFortMcpProfileAccount* AccountProfile = PlayerInfo ? static_cast<UFortMcpProfileAccount*>(PlayerInfo->AccountProfile) : nullptr;
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    if (Role != ROLE_Authority || !Inventory || !Bars || !AccountProfile || !GameData || !Finder::ResolveHomebaseGadgetsLive())
    {
        return;
    }

    const FFortSavedModeLoadout ModeLoadout = AccountProfile->GetModeLoadout(FString(L"Default"));
    const TArray<FString>& SelectedGadgets = ModeLoadout.SelectedGadgets;
    const TAssetPtr<UFortHomebaseManager>& HomebaseManagerPtr = GameData->HomebaseManager;
    UFortHomebaseManager* HomebaseManager = SelectedGadgets.Num() > 0 ? HomebaseManagerPtr.LoadSynchronous() : nullptr;
    if (!HomebaseManager)
    {
        return;
    }

    TStructArray<FFortItemEntry> SelectableGadgets;
    HomebaseManager->GetSelectableGadgetItems(AccountProfile, Natives::UFortRegisteredPlayerInfo_GetAbilitySystemInterface(PlayerInfo), SelectableGadgets);
    TArray<UFortAbilityKit*> Kits;
    HomebaseManager->GetOwnedNodeAbilityKits(AccountProfile, Kits);

    std::vector<int32_t> GadgetSlots;
    const int32_t NumSlots = Bars->GetQuickBar(EFortQuickBars::GetPrimary()).Slots.Num();
    for (int32_t SlotIndex = 0; SlotIndex < NumSlots; ++SlotIndex)
    {
        const TArray<EFortItemType>& AcceptedItemTypes = Bars->GetSlotData(EFortQuickBars::GetPrimary(), SlotIndex).AcceptedItemTypes;
        if (AcceptedItemTypes.Contains(EFortItemType::GetHomebaseGadget()))
        {
            GadgetSlots.push_back(SlotIndex);
        }
    }

    FHeroAbilityKitItems& Granted = GetHeroAbilityKitItems(this);
    for (int32_t Index = 0; Index < SelectedGadgets.Num(); ++Index)
    {
        const FString& Selected = SelectedGadgets[Index];
        if (Selected.IsEmpty())
        {
            continue;
        }

        const FFortItemEntry* GadgetEntry = nullptr;
        for (int32_t EntryIndex = 0; EntryIndex < SelectableGadgets.Num() && !GadgetEntry; ++EntryIndex)
        {
            const UFortItemDefinition* Definition = SelectableGadgets[EntryIndex].ItemDefinition;
            if (Definition && _wcsicmp(*Definition->GetPersistentName(), *Selected) == 0)
            {
                GadgetEntry = &SelectableGadgets[EntryIndex];
            }
        }
        UFortWorldItemDefinition* Gadget = GadgetEntry ? Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(GadgetEntry->ItemDefinition)) : nullptr;
        if (!Gadget)
        {
            UE_LOG(LogFort, Warning, TEXT("ApplyHomebaseGadgets: %s selected gadget '%s' is not unlocked"), *GetName(), *Selected);
            continue;
        }

        const bool bWasGranted = std::any_of(Granted.GrantedHomebaseGadgets.begin(), Granted.GrantedHomebaseGadgets.end(), [Gadget](const TWeakObjectPtr<UFortItemDefinition>& Definition) { return Definition.Get() == Gadget; });
        if (bWasGranted || GetItemQuantity(Gadget) > 0)
        {
            continue;
        }
        Granted.GrantedHomebaseGadgets.emplace_back(Gadget);

        const int32_t Level = GadgetEntry->Level;
        const int32_t Slot = Index < static_cast<int32_t>(GadgetSlots.size()) && Bars->GetSlot(EFortQuickBars::GetPrimary(), GadgetSlots[Index]).Items.Num() == 0 ? GadgetSlots[Index] : -1;
        for (UFortWorldItem* GadgetItem : AddHeroAbilityKitItem(this, Inventory, Gadget, 1, Level))
        {
            FFortItemEntry& ItemEntry = GadgetItem->ItemEntry;
            Bars->ServerAddItemInternal(ItemEntry.ItemGuid, EFortQuickBars::GetPrimary(), Slot);
        }

        for (UFortAbilityKit* Kit : Kits)
        {
            const std::vector<UFortGadgetItemDefinition*> KitGadgetDefinitions = Kit ? KitGadgets(Kit) : std::vector<UFortGadgetItemDefinition*>{};
            if (std::none_of(KitGadgetDefinitions.begin(), KitGadgetDefinitions.end(), [Gadget](UFortGadgetItemDefinition* KitGadget) { return static_cast<UFortItemDefinition*>(KitGadget) == Gadget; }))
            {
                continue;
            }

            TStructArray<FAbilityKitItem>& Items = Kit->Items;
            for (int32_t ItemIndex = 0; ItemIndex < Items.Num(); ++ItemIndex)
            {
                const FAbilityKitItem& KitItem = Items[ItemIndex];
                UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(KitItem.Item));
                const int32_t Count = ItemDefinition && ItemDefinition->ItemType != EFortItemType::GetWeaponHarvest() ? AbilityKitItemCount(KitItem, GetItemQuantity(ItemDefinition)) : 0;
                if (Count > 0)
                {
                    AddHeroAbilityKitItem(this, Inventory, ItemDefinition, Count, Level);
                }
            }
        }
        UE_LOG(LogFort, Log, TEXT("ApplyHomebaseGadgets: %s gets '%s' at level %d, quickbar slot %d"), *GetName(), *Selected, Level, Slot);
    }

    for (int32_t Index = 0; Index < SelectableGadgets.Num(); ++Index)
    {
        FFortItemEntry::StaticStruct()->DestroyStruct(&SelectableGadgets[Index]);
    }
    FMemory::Free(SelectableGadgets.GetData());
#endif
}

void AFortPlayerController::AddHeroAbilityKitGadgetsToQuickBars()
{
#if WITH_SERVER_CODE
    AFortQuickBars* Bars = QuickBars;
    AFortInventory* Inventory = WorldInventory;
    FHeroAbilityKitItems& Granted = GetHeroAbilityKitItems(this);
    const std::vector<FGuid> Pending = std::move(Granted.PendingQuickBarItems);
    Granted.PendingQuickBarItems.clear();
    for (const FGuid& ItemGuid : Pending)
    {
        UFortWorldItem* GadgetItem = Bars && Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
        if (!GadgetItem || Bars->ContainsItem(ItemGuid))
        {
            continue;
        }

        FFortItemEntry& GadgetEntry = GadgetItem->ItemEntry;
        UFortWorldItemDefinition* Gadget = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(GadgetEntry.ItemDefinition));
        FQuickBar& PrimaryQuickBar = Bars->PrimaryQuickBar;
        const int32_t PreferredSlot = Gadget ? static_cast<int32_t>(Gadget->PreferredQuickbarSlot) : -1;
        const TStructArray<FQuickBarSlot>& Slots = PrimaryQuickBar.Slots;
        Bars->ServerAddItemInternal(ItemGuid, EFortQuickBars::GetPrimary(), PreferredSlot >= 0 && PreferredSlot < Slots.Num() ? PreferredSlot : -1);
    }
#endif
}

void AFortPlayerController::ServerExecuteInventoryItem_Implementation(const FGuid& ItemGuid)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    if (UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition)))
    {
        ItemDefinition->ServerExecute(WorldItem, this);
    }
#endif
}

void AFortPlayerController::ServerSpawnInventoryDrop_Implementation(const FGuid& ItemGuid, int32_t Count)
{
#if WITH_SERVER_CODE
    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    AFortPawn* Pawn = MyFortPawn;
    if (!WorldItem || !Pawn || Count <= 0)
    {
        return;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    UFortWorldItemDefinition* ItemDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    if (!ItemDefinition || !ItemDefinition->bCanBeDropped)
    {
        return;
    }

    const int32_t DropCount = std::min(Count, static_cast<int32_t>(ItemEntry.Count));
    WorldItem->SetDurability(ItemEntry.Durability);
    UScriptStruct* EntryStruct = FFortItemEntry::StaticStruct();
    std::vector<uint8_t> EntryStorage(EntryStruct->GetStructureSize(), 0);
    FFortItemEntry* DropEntry = reinterpret_cast<FFortItemEntry*>(EntryStorage.data());
    EntryStruct->InitializeStruct(DropEntry);
    EntryStruct->CopyScriptStruct(DropEntry, &ItemEntry);
    DropEntry->Count = DropCount;

    if (RemoveInventoryItem(ItemGuid, DropCount, false))
    {
        const FTransform PawnTransform = Pawn->GetTransform();
        const FVector Location = PawnTransform.GetLocation();
        if (AFortPickup* Pickup = SpawnDropPickup(*DropEntry, &Location, nullptr))
        {
            constexpr float TossDistance = 512.0f;
            const float Yaw = PawnTransform.Rotation.Rotator().Yaw * (3.1415926535897932f / 180.0f);
            const FVector TossLocation(Location.X + std::cos(Yaw) * TossDistance, Location.Y + std::sin(Yaw) * TossDistance, Location.Z);
            Pickup->PawnWhoDroppedPickup = Pawn;
            Pickup->TossPickup(TossLocation, Pawn, 0);
        }
    }
    EntryStruct->DestroyStruct(DropEntry);
#endif
}

void AFortPlayerController::ServerSpotActor_Implementation(AActor* NewlySpottedActor)
{
#if WITH_SERVER_CODE
    UClass* const SpottableActorInterface = UFortSpottableActorInterface::StaticClass();
    if (!NewlySpottedActor || !Finder::ResolveSpottingLive(SpottableActorInterface))
    {
        return;
    }

    IFortSpottableActorInterface* Spottable = static_cast<IFortSpottableActorInterface*>(Natives::UObject_GetInterfaceAddress(NewlySpottedActor, SpottableActorInterface));
    if (!Spottable || !Spottable->CanBeSpottedBy(this))
    {
        return;
    }

    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    if (AFortTeamInfo* Team = FortPlayerState ? static_cast<AFortTeamInfo*>(FortPlayerState->PlayerTeam) : nullptr)
    {
        Team->AddSpottedActor(this, NewlySpottedActor);
    }

    AFortPawn* SpotterPawn = MyFortPawn;
    AFortPawn* SpottedPawn = Cast<AFortPawn>(NewlySpottedActor);
    if (SpotterPawn && SpottedPawn)
    {
        SpotterPawn->TriggerFeedbackEvent(SpottedPawn->SpottedEvent, SpotterPawn, SpottedPawn, 0.f);
    }
#endif
}

void AFortPlayerController::ServerRemoveInventoryStateValue_Implementation(const FGuid& ItemGuid, EFortItemEntryState StateType)
{
    ServerRemoveInventoryStateValue_ImplementationOG(this, ItemGuid, StateType);

#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return;
    }

    for (AFortInventory* Inventory : { static_cast<AFortInventory*>(WorldInventory), static_cast<AFortInventory*>(OutpostInventory) })
    {
        if (UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr)
        {
            static_cast<FFortItemEntry&>(WorldItem->ItemEntry).RemoveStateValue(StateType);
            return;
        }
    }
#endif
}

AFortPickup* AFortPlayerController::SpawnDropPickup(const FFortItemEntry& ItemEntry, const FVector* SpawnLocation, const FRotator* SpawnRotation)
{
    if (Natives::AFortPlayerController_SpawnDropPickupIndex < 0 && !Finder::ResolvePlayerDropPickupLive())
    {
        return nullptr;
    }
    return CallVirtual<AFortPickup*>(this, Natives::AFortPlayerController_SpawnDropPickupIndex, &ItemEntry, SpawnLocation, SpawnRotation);
}

bool AFortPlayerController::RemoveInventoryItem(const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
    void* InventoryOwner = reinterpret_cast<uint8_t*>(this) + Natives::AFortPlayerController_InventoryOwnerInterface;
    const bool bRemoved = RemoveInventoryItemOG(InventoryOwner, ItemGuid, Count, bForceRemoveFromQuickBars);

#if WITH_SERVER_CODE
    if (Role != ROLE_Authority)
    {
        return bRemoved;
    }

    AFortInventory* Inventory = WorldInventory;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    if (!WorldItem)
    {
        return false;
    }

    FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
    const int32_t Remaining = ItemEntry.Count - Count;
    UFortAmmoItemDefinition* AmmoDefinition = Cast<UFortAmmoItemDefinition>(static_cast<UFortItemDefinition*>(ItemEntry.ItemDefinition));
    if (Count >= 0 && Remaining <= 0 && AmmoDefinition && AmmoDefinition->RegenCooldown > 0.0f)
    {
        Inventory->UpdateWorldItemCount(WorldItem, 0);
    }
    else if (Count < 0 || Remaining <= 0)
    {
        if (AFortQuickBars* Bars = QuickBars)
        {
            Bars->ServerRemoveItemInternal(ItemGuid, false, bForceRemoveFromQuickBars);
        }
        Inventory->RemoveWorldItem(WorldItem);
    }
    else
    {
        Inventory->UpdateWorldItemCount(WorldItem, Remaining);
    }
    return true;
#else
    return bRemoved;
#endif
}

bool AFortPlayerController::ModDurability(const FGuid& ItemGuid, float DurabilityMod, bool bForceSet)
{
#if WITH_SERVER_CODE
    if (Role == ROLE_Authority && !(DurabilityMod < 0.0f && CheatManager && bInfiniteDurability))
    {
        AFortInventory* Inventory = WorldInventory;
        UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
        UFortWorldItemDefinition* ItemDefinition = WorldItem ? Cast<UFortWorldItemDefinition>(WorldItem->GetItemDefinitionBP()) : nullptr;
        if (!ItemDefinition || !WorldItem->HasDurability())
        {
            return false;
        }

        FFortItemEntry& ItemEntry = WorldItem->ItemEntry;
        const float MaxDurability = ItemDefinition->GetMaxDurability(WorldItem->GetLevel());
        const float NewDurability = std::clamp(bForceSet ? DurabilityMod : ItemEntry.Durability + DurabilityMod, 0.0f, std::max(MaxDurability, 0.0f));
        WorldItem->SetDurability(NewDurability);
        if (NewDurability <= 0.0f)
        {
            HandleItemZeroDurability(ItemGuid);
        }
        return true;
    }
#endif
    void* InventoryOwner = reinterpret_cast<uint8_t*>(this) + Natives::AFortPlayerController_InventoryOwnerInterface;
    return ModDurabilityOG(InventoryOwner, ItemGuid, DurabilityMod, bForceSet);
}

bool AFortPlayerController::HandleItemZeroDurability(const FGuid& ItemGuid)
{
#if WITH_SERVER_CODE
    AFortPawn* Pawn = MyFortPawn;
    AFortWeapon* Weapon = Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    const bool bEquipped = Weapon && static_cast<FGuid&>(Weapon->ItemEntryGuid) == ItemGuid;
    if (bEquipped)
    {
        Weapon->DestroyedDueToDurability();
    }
    const bool bRemoved = RemoveInventoryItem(ItemGuid, -1, true);
    if (bEquipped && Weapon->IsA(AFortWeaponRanged::StaticClass()))
    {
        GameThread::Enqueue([PlayerController = TWeakObjectPtr<AFortPlayerController>(this), BrokenWeapon = TWeakObjectPtr<AFortWeapon>(Weapon)]()
        {
            AFortPlayerController* Controller = PlayerController.Get();
            AFortWeapon* Broken = BrokenWeapon.Get();
            AFortPawn* ControllerPawn = Controller ? static_cast<AFortPawn*>(Controller->MyFortPawn) : nullptr;
            if (Broken && ControllerPawn && static_cast<AFortWeapon*>(ControllerPawn->CurrentWeapon) == Broken)
            {
                Controller->EndWeaponAbilities(Broken);
            }
        });
    }
    return bRemoved;
#else
    return false;
#endif
}

void AFortPlayerController::ServerDisassembleInventoryItem_Implementation(const FGuid& ItemGuid)
{
    DisassembleInventoryItem(ItemGuid);
}

bool AFortPlayerController::DisassembleInventoryItem(const FGuid& ItemGuid)
{
    if (!Natives::AFortPlayerController_DisassembleInventoryItem(this, ItemGuid))
    {
        return false;
    }

#if WITH_SERVER_CODE
    AFortInventory* Inventory = Role == ROLE_Authority ? static_cast<AFortInventory*>(WorldInventory) : nullptr;
    UFortWorldItem* WorldItem = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
    UFortWorldItemDefinition* ItemDefinition = WorldItem ? Cast<UFortWorldItemDefinition>(WorldItem->GetItemDefinitionBP()) : nullptr;
    if (!ItemDefinition)
    {
        return true;
    }

    TStructArray<FFortItemEntry> Results;
    ItemDefinition->GetDisassemblyResults(Results, WorldItem->GetNormalizedDurability());
    if (RemoveInventoryItem(ItemGuid, 1, false))
    {
        AFortPawn* Pawn = MyFortPawn;
        for (int32_t Index = 0; Index < Results.Num(); ++Index)
        {
            FFortItemEntry& Result = Results[Index];
            UFortWorldItemDefinition* ResultDefinition = Cast<UFortWorldItemDefinition>(static_cast<UFortItemDefinition*>(Result.ItemDefinition));
            const int32_t Count = Result.Count;
            if (!ResultDefinition || Count <= 0)
            {
                continue;
            }

            const int32_t Given = Inventory->AddItemStacks(Result, Count - Inventory->GetOverflowAmount(Result, false), -1, true, true,
                [Inventory](UFortWorldItem* Existing, int32_t Added)
                {
                    FFortItemEntry& ExistingEntry = Existing->ItemEntry;
                    Inventory->UpdateWorldItemCount(Existing, ExistingEntry.Count + Added);
                    ExistingEntry.AddNewItemCount(Added, false);
                },
                [&](int32_t StackCount, bool bOverflow) -> UFortWorldItem*
                {
                    UFortWorldItem* NewItem = Cast<UFortWorldItem>(ResultDefinition->CreateTemporaryItemInstanceBP(StackCount, Result.Level));
                    if (!NewItem)
                    {
                        return nullptr;
                    }

                    FFortItemEntry& NewEntry = NewItem->ItemEntry;
                    NewEntry.inventory_overflow_date = bOverflow;
                    NewItem->SetOwningControllerForTemporaryItem(this);
                    Inventory->AddWorldItem(NewItem);
                    NewEntry.SetNewItemCount(StackCount, false);
                    if (!bOverflow)
                    {
                        TryAddToQuickBar(NewItem);
                    }
                    return NewItem;
                });

            if (Given < Count && Pawn)
            {
                Result.Count = Count - Given;
                const FVector Location = Pawn->GetTransform().GetLocation();
                if (AFortPickup* Pickup = AFortPickup::CreatePickup(GetWorld(), Result, &Location))
                {
                    Pickup->TossPickup(Location, Pawn, 0);
                }
            }
        }
    }

    for (int32_t Index = 0; Index < Results.Num(); ++Index)
    {
        FFortItemEntry::StaticStruct()->DestroyStruct(&Results[Index]);
    }
    FMemory::Free(Results.GetData());
#endif
    return true;
}

void AFortPlayerController::EndWeaponAbilities(AFortWeapon* Weapon)
{
#if WITH_SERVER_CODE
    AFortPlayerState* FortPlayerState = Cast<AFortPlayerState>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(PlayerState)));
    UAbilitySystemComponent* AbilitySystemComponent = FortPlayerState ? static_cast<UAbilitySystemComponent*>(FortPlayerState->AbilitySystemComponent) : nullptr;
    if (!AbilitySystemComponent || !Weapon)
    {
        return;
    }

    std::vector<TWeakObjectPtr<UGameplayAbility>> ActiveInstances;
    FGameplayAbilitySpecContainer& Container = AbilitySystemComponent->ActivatableAbilities;
    TStructArray<FGameplayAbilitySpec>& Specs = Container.Items;
    for (int32_t Index = 0; Index < Specs.Num(); ++Index)
    {
        FGameplayAbilitySpec& Spec = Specs[Index];
        UObject* SourceObject = Spec.SourceObject;
        const uint8_t ActiveCount = Spec.ActiveCount;
        if (SourceObject != Weapon || ActiveCount == 0)
        {
            continue;
        }
        TArray<UGameplayAbility*>& ReplicatedInstances = Spec.ReplicatedInstances;
        TArray<UGameplayAbility*>& NonReplicatedInstances = Spec.NonReplicatedInstances;
        for (TArray<UGameplayAbility*>* Instances : { &ReplicatedInstances, &NonReplicatedInstances })
        {
            for (UGameplayAbility* Instance : *Instances)
            {
                if (Instance)
                {
                    ActiveInstances.emplace_back(Instance);
                }
            }
        }
    }

    for (const TWeakObjectPtr<UGameplayAbility>& Instance : ActiveInstances)
    {
        if (UGameplayAbility* Ability = Instance.Get())
        {
            Ability->K2_EndAbility();
        }
    }
#endif
}

void AFortPlayerController::HookVTables()
{
    UObject* PlayerControllerDefault = StaticClass()->GetDefaultObject(false);
    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;

    const int32_t PossessIndex = Finder::ImplementationIndex(AController::StaticFunction_Possess());
    void* const PlayerControllerPossess = PlayerControllerVTable && PossessIndex >= 0 ? PlayerControllerVTable[PossessIndex] : nullptr;
    if (PlayerControllerPossess)
    {
        PossessOG = reinterpret_cast<decltype(PossessOG)>(PlayerControllerPossess);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[PossessIndex] == PlayerControllerPossess)
            {
                Hooks::SwapVTable(L"AFortPlayerController::Possess", VTable, PossessIndex, reinterpret_cast<void*>(&PossessHK));
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::Possess slot not found");
    }
    if (UFunction* ActivateSlotFunction = StaticFunction_ActivateSlot(); ActivateSlotFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        ActivateSlotFunction->SetNativeFunc(&execActivateSlot);
        Log::Info(L"Hook AFortPlayerController::ActivateSlot: native");
    }
    if (UFunction* GivePlayerAmmoFunction = StaticFunction_GivePlayerAmmo(); GivePlayerAmmoFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        GivePlayerAmmoFunction->SetNativeFunc(&execGivePlayerAmmo);
        Log::Info(L"Hook AFortPlayerController::GivePlayerAmmo: native");
    }
    if (UFunction* SwapQuickBarFocusFunction = StaticFunction_SwapQuickBarFocus(); SwapQuickBarFocusFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        SwapQuickBarFocusFunction->SetNativeFunc(&execSwapQuickBarFocus);
        Log::Info(L"Hook AFortPlayerController::SwapQuickBarFocus: native");
    }

    if (Finder::ResolveInventoryOwnerLive(StaticFunction_ServerRemoveInventoryItem(), PlayerControllerDefault, Member_WorldInventory.GetOffsetChecked()))
    {
        const ptrdiff_t InterfaceOffset = Natives::AFortPlayerController_InventoryOwnerInterface;
        const int32_t RemoveIndex = Natives::IFortInventoryOwnerInterface_RemoveInventoryItemIndex;
        void* const PlayerControllerRemove = (*reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(PlayerControllerDefault) + InterfaceOffset))[RemoveIndex];
        RemoveInventoryItemOG = reinterpret_cast<decltype(RemoveInventoryItemOG)>(PlayerControllerRemove);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** InterfaceVTable = ClassDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(ClassDefault) + InterfaceOffset) : nullptr;
            if (InterfaceVTable && InterfaceVTable[RemoveIndex] == PlayerControllerRemove)
            {
                Hooks::SwapVTable(L"AFortPlayerController::RemoveInventoryItem", InterfaceVTable, RemoveIndex, reinterpret_cast<void*>(&RemoveInventoryItemHK));
            }
        }

        if (ResolveDurability())
        {
            const int32_t ModIndex = Natives::IFortInventoryOwnerInterface_ModDurabilityIndex;
            void* const PlayerControllerMod = (*reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(PlayerControllerDefault) + InterfaceOffset))[ModIndex];
            ModDurabilityOG = reinterpret_cast<decltype(ModDurabilityOG)>(PlayerControllerMod);
            for (TObjectIterator<UClass> It; It; ++It)
            {
                UClass* Class = *It;
                UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
                void** InterfaceVTable = ClassDefault ? *reinterpret_cast<void***>(reinterpret_cast<uint8_t*>(ClassDefault) + InterfaceOffset) : nullptr;
                if (InterfaceVTable && InterfaceVTable[ModIndex] == PlayerControllerMod)
                {
                    Hooks::SwapVTable(L"AFortPlayerController::ModDurability", InterfaceVTable, ModIndex, reinterpret_cast<void*>(&ModDurabilityHK));
                }
            }
        }
    }

    if (const int32_t DisassembleIndex = Finder::ImplementationIndex(StaticFunction_ServerDisassembleInventoryItem());
        PlayerControllerVTable && DisassembleIndex >= 0 && Finder::ResolveDisassemblyLive(StaticFunction_ServerDisassembleInventoryItem(), PlayerControllerDefault))
    {
        void* const PlayerControllerDisassemble = PlayerControllerVTable[DisassembleIndex];
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[DisassembleIndex] == PlayerControllerDisassemble)
            {
                Hooks::SwapVTable(L"AFortPlayerController::ServerDisassembleInventoryItem_Implementation", VTable, DisassembleIndex, reinterpret_cast<void*>(&ServerDisassembleInventoryItem_ImplementationHK));
            }
        }
    }

    if (const int32_t StateValueIndex = Finder::ImplementationIndex(StaticFunction_ServerRemoveInventoryStateValue()); PlayerControllerVTable && StateValueIndex >= 0)
    {
        void* const PlayerControllerRemoveStateValue = PlayerControllerVTable[StateValueIndex];
        ServerRemoveInventoryStateValue_ImplementationOG = reinterpret_cast<decltype(ServerRemoveInventoryStateValue_ImplementationOG)>(PlayerControllerRemoveStateValue);
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[StateValueIndex] == PlayerControllerRemoveStateValue)
            {
                Hooks::SwapVTable(L"AFortPlayerController::ServerRemoveInventoryStateValue_Implementation", VTable, StateValueIndex, reinterpret_cast<void*>(&ServerRemoveInventoryStateValue_ImplementationHK));
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::ServerRemoveInventoryStateValue_Implementation slot not found");
    }

    if (const int32_t CheatIndex = Finder::ImplementationIndex(StaticFunction_ServerCheat());
        PlayerControllerVTable && CheatIndex >= 0 && Finder::ResolvePlayerControllerConsoleCommand(reinterpret_cast<uintptr_t>(PlayerControllerVTable[CheatIndex])))
    {
        void* const PlayerControllerCheat = PlayerControllerVTable[CheatIndex];
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[CheatIndex] == PlayerControllerCheat)
            {
                Hooks::SwapVTable(L"AFortPlayerController::ServerCheat_Implementation", VTable, CheatIndex, reinterpret_cast<void*>(&ServerCheat_ImplementationHK));
            }
        }
    }
    else
    {
        Log::Error(L"Finder: AFortPlayerController::ServerCheat_Implementation slot not found");
    }

    for (const auto& [Function, Implementation, Name] : {
             std::tuple{ StaticFunction_ServerExecuteInventoryItem(), reinterpret_cast<void*>(&ServerExecuteInventoryItem_ImplementationHK), L"AFortPlayerController::ServerExecuteInventoryItem_Implementation" },
             std::tuple{ StaticFunction_ServerSpawnInventoryDrop(), reinterpret_cast<void*>(&ServerSpawnInventoryDrop_ImplementationHK), L"AFortPlayerController::ServerSpawnInventoryDrop_Implementation" },
             std::tuple{ StaticFunction_ServerSpotActor(), reinterpret_cast<void*>(&ServerSpotActor_ImplementationHK), L"AFortPlayerController::ServerSpotActor_Implementation" },
         })
    {
        const int32_t Index = Finder::ImplementationIndex(Function);
        if (Index < 0)
        {
            Log::Error(L"Finder: {} slot not found", Name);
            continue;
        }

        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
            {
                Hooks::SwapVTable(Name, VTable, Index, Implementation);
            }
        }
    }
}

int32_t AFortPlayerController::GetItemQuantity(const UFortItemDefinition* ItemDefinition)
{
    AFortInventory* Inventory = WorldInventory;
    if (!Inventory || !ItemDefinition)
    {
        return 0;
    }

    int32_t Quantity = 0;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        FFortItemEntry& Entry = Instances[Index]->ItemEntry;
        if (static_cast<UFortItemDefinition*>(Entry.ItemDefinition) == ItemDefinition)
        {
            Quantity += Entry.Count;
        }
    }
    return Quantity;
}

ptrdiff_t AFortPlayerController::CriticalMatchBonusOffset()
{
    return Member_bShouldReceiveCriticalMatchBonus.GetOffsetChecked();
}

bool AFortPlayerController::ResolveDurability()
{
    return Finder::ResolveDurabilityLive(StaticFunction_ServerModDurability(), StaticClass()->GetDefaultObject(false), FFortItemEntry::DurabilityOffset(),
        AFortWeapon::PendingDestroyDueToDurabilityOffset(), AFortWeapon::PendingDestroyDueToDurabilityMask());
}

UFortQuestManager* AFortPlayerController::GetQuestManager() const
{
    UFortRegisteredPlayerInfo* PlayerInfo = MyPlayerInfo;
    return PlayerInfo ? static_cast<UFortQuestManager*>(PlayerInfo->QuestManager) : nullptr;
}

UFortSchematicItem* AFortPlayerController::GetSchematicForItem(const UFortItemDefinition* ItemDefinition)
{
    if (!Natives::AFortPlayerController_GetSchematicForItem && !Finder::ResolveCraftingLive())
    {
        return nullptr;
    }
    return Natives::AFortPlayerController_GetSchematicForItem(this, ItemDefinition);
}

void AFortPlayerController::ActivateSlot(EFortQuickBars InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot)
{
    if (AFortQuickBars* Bars = QuickBars)
    {
        Bars->ServerActivateSlotInternal(InQuickBar, Slot, ActivateDelay, bUpdatePreviousFocusedSlot);
    }
}

DEFINE_FUNCTION(AFortPlayerController::execActivateSlot)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_InQuickBar);
    P_GET_PROPERTY(UIntProperty, Z_Param_Slot);
    P_GET_PROPERTY(UFloatProperty, Z_Param_ActivateDelay);
    P_GET_UBOOL(Z_Param_bUpdatePreviousFocusedSlot);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->ActivateSlot(EFortQuickBars{ Z_Param_InQuickBar }, Z_Param_Slot, Z_Param_ActivateDelay, Z_Param_bUpdatePreviousFocusedSlot);
    P_NATIVE_END;
}

void AFortPlayerController::GivePlayerAmmo(int32_t AmmoCount)
{
#if WITH_SERVER_CODE
    AFortPawn* Pawn = MyFortPawn;
    AFortWeapon* Weapon = Role == ROLE_Authority && Pawn ? static_cast<AFortWeapon*>(Pawn->CurrentWeapon) : nullptr;
    if (!Weapon || AmmoCount <= 0)
    {
        return;
    }

    const int32_t BulletsPerClip = Weapon->GetBulletsPerClip();
    const int32_t Refunded = std::min(AmmoCount, BulletsPerClip - static_cast<int32_t>(Weapon->AmmoCount));
    if (Refunded > 0)
    {
        Weapon->ModAmmo(Refunded);
    }
#endif
}

DEFINE_FUNCTION(AFortPlayerController::execGivePlayerAmmo)
{
    P_GET_PROPERTY(UIntProperty, Z_Param_AmmoCount);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->GivePlayerAmmo(Z_Param_AmmoCount);
    P_NATIVE_END;
}

void AFortPlayerController::SwapQuickBarFocus(EFortQuickBars InQuickBar)
{
#if WITH_SERVER_CODE
    AFortQuickBars* Bars = QuickBars;
    if (Bars && Bars->GetCurrentQuickBar() != InQuickBar)
    {
        Bars->SwapQuickBarFocus(InQuickBar, -1);
    }
#endif
}

void AFortPlayerController::ClientReportDamagedResourceBuilding(ABuildingSMActor* BuildingSMActor, uint8_t PotentialResourceType, int32_t PotentialResourceCount, bool bDestroyed, bool bJustHitWeakspot)
{
    // 1.2 has no thunk for this client RPC left; send it through ProcessEvent.
    Reflection::FParams Params(StaticFunction_ClientReportDamagedResourceBuilding());
    Params.Set(L"BuildingSMActor", BuildingSMActor);
    Params.Set(L"PotentialResourceType", PotentialResourceType);
    Params.Set(L"PotentialResourceCount", PotentialResourceCount);
    Params.Set(L"bDestroyed", bDestroyed);
    Params.Set(L"bJustHitWeakspot", bJustHitWeakspot);
    Params.Call(this);
}

DEFINE_FUNCTION(AFortPlayerController::execSwapQuickBarFocus)
{
    P_GET_PROPERTY(UByteProperty, Z_Param_InQuickBar);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->SwapQuickBarFocus(EFortQuickBars{ Z_Param_InQuickBar });
    P_NATIVE_END;
}

void AFortPlayerController::Cheat(const FString& Msg)
{
    // The native Cheat (Cert 0x740C20, 1.2 0x140737A00, 1.7.2 0x7F7C40) sends the command through the matchmaking lobby
    // beacon whenever the client still has one (UFortGameInstance::Matchmaking->LobbyBeaconClient), which a Save the
    // World player keeps for the whole match. The server's ALobbyBeaconClient::ServerCheat_Implementation is empty in
    // these builds, so every "cheat" was dropped without a word (#75). It goes to this controller's ServerCheat instead.
    ServerCheat(Msg);
}

DEFINE_FUNCTION(AFortPlayerController::execCheat)
{
    P_GET_PROPERTY(UStrProperty, Z_Param_Msg);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<AFortPlayerController*>(Context)->Cheat(Z_Param_Msg);
    P_NATIVE_END;
}

void AFortPlayerController::ServerCheat(const FString& Msg)
{
    Reflection::FParams Params(StaticFunction_ServerCheat());
    Params.Set(L"Msg", Msg);
    Params.Call(this);
}

void AFortPlayerController::ServerCheat_Implementation(const FString& Msg)
{
#if WITH_SERVER_CODE
    // Cheats are on for every player (#65, #69); a controller that came another way gets its cheat manager here.
    if (!CheatManager)
    {
        AddCheats(true);
    }
    UE_LOG(LogFort, Log, TEXT("ServerCheat: %s runs \"%s\" (cheat manager %s)"), *GetNameSafe(this), *Msg, *GetNameSafe(static_cast<UObject*>(CheatManager)));

    // The native body runs ConsoleCommand(Msg, true), which writes the output to the server's log and replies with an
    // empty ClientMessage. Collecting it instead sends the player what the command printed, "Command not recognized"
    // included; the cheats' own replies ("God mode on") are ClientMessages of their own.
    FString Result = ConsoleCommand(Msg, false);
    while (!Result.IsEmpty() && (Result.EndsWith(L"\n") || Result.EndsWith(L"\r")))
    {
        Result = Result.Left(Result.Len() - 1);
    }
    if (!Result.IsEmpty())
    {
        UE_LOG(LogFort, Log, TEXT("ServerCheat: %s"), *Result);
        ClientMessage(Result);
    }
#endif
}

AFortPlayerController* AFortPlayerController::FromInventoryOwner(IFortInventoryOwnerInterface* InventoryOwner)
{
    if (!InventoryOwner || Natives::AFortPlayerController_InventoryOwnerInterface < 0)
    {
        return nullptr;
    }
    return reinterpret_cast<AFortPlayerController*>(reinterpret_cast<uint8_t*>(InventoryOwner) - Natives::AFortPlayerController_InventoryOwnerInterface);
}

bool AFortPlayerController::RemoveInventoryItemHK(void* InventoryOwner, const FGuid& ItemGuid, int32_t Count, bool bForceRemoveFromQuickBars)
{
    AFortPlayerController* PlayerController = FromInventoryOwner(static_cast<IFortInventoryOwnerInterface*>(InventoryOwner));
    return PlayerController->RemoveInventoryItem(ItemGuid, Count, bForceRemoveFromQuickBars);
}

bool AFortPlayerController::ModDurabilityHK(void* InventoryOwner, const FGuid& ItemGuid, float DurabilityMod, bool bForceSet)
{
    AFortPlayerController* PlayerController = FromInventoryOwner(static_cast<IFortInventoryOwnerInterface*>(InventoryOwner));
    return PlayerController->ModDurability(ItemGuid, DurabilityMod, bForceSet);
}

void AFortPlayerController::ServerDisassembleInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid)
{
    This->ServerDisassembleInventoryItem_Implementation(ItemGuid);
}

void AFortPlayerController::PossessHK(AFortPlayerController* This, APawn* InPawn)
{
    This->Possess(InPawn);
}

void AFortPlayerController::TrySetHeroImplementationHK(AFortPlayerController* This, UFortHero* NewHero, bool bSendToServer, bool bUnused)
{
    This->TrySetHeroImplementation(NewHero, bSendToServer, bUnused);
}

void AFortPlayerController::ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid)
{
    This->ServerExecuteInventoryItem_Implementation(ItemGuid);
}

void AFortPlayerController::ServerSpotActor_ImplementationHK(AFortPlayerController* This, AActor* NewlySpottedActor)
{
    This->ServerSpotActor_Implementation(NewlySpottedActor);
}

void AFortPlayerController::ServerCheat_ImplementationHK(AFortPlayerController* This, const FString& Msg)
{
    This->ServerCheat_Implementation(Msg);
}

void AFortPlayerController::ServerRemoveInventoryStateValue_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, EFortItemEntryState StateType)
{
    This->ServerRemoveInventoryStateValue_Implementation(ItemGuid, StateType);
}

void AFortPlayerController::ServerSpawnInventoryDrop_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid, int32_t Count)
{
    This->ServerSpawnInventoryDrop_Implementation(ItemGuid, Count);
}

void AFortPlayerController::HookClientNatives()
{
    if (UFunction* CheatFunction = StaticFunction_Cheat(); CheatFunction && (Natives::FFrame_Step || Finder::ResolveScriptFrame()))
    {
        CheatFunction->SetNativeFunc(&execCheat);
        Log::Info(L"Hook AFortPlayerController::Cheat: native");
    }
}

void AFortPlayerController::Hook()
{
    Hooks::Detour(L"AFortPlayerController::InitializeMcpProfile", reinterpret_cast<void*>(Natives::AFortPlayerController_InitializeMcpProfile), &InitializeMcpProfileHK, &InitializeMcpProfileOG);
    Hooks::Detour(L"AFortPlayerController::TrySetHeroImplementation", reinterpret_cast<void*>(Natives::AFortPlayerController_TrySetHeroImplementation), &TrySetHeroImplementationHK, &TrySetHeroImplementationOG);
}

bool AFortPlayerController::InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete)
{
    return This->InitializeMcpProfile(OnComplete);
}
