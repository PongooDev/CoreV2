#include "pch.h"
#include "Player/FortPlayerControllerZone.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AttributeSet.h"
#include "Athena/Mutators/FortAthenaMutator_GiveItemsAtGamePhaseStep.h"
#include "Engine/World.h"
#include "Finder.h"
#include "FortGameState.h"
#include "FortRuntimeOptions.h"
#include "GameFramework/PlayerState.h"
#include "Hooks.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Missions/FortMissionLibrary.h"
#include "Natives.h"
#include "Player/FortCheatManager.h"
#include "Player/FortPlayerState.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

void AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation()
{
#if WITH_SERVER_CODE
    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    if (!GameState || GameState->GameplayState != EFortGameplayState::GetEndOfZone() || SetupTeleportOutCameraGameState.Get() == GameState)
    {
        return;
    }

    SetupTeleportOutCameraGameState = FWeakObjectPtr(GameState);
    UFortMissionLibrary::ExecuteMissionEvent(this, FName(L"SetupTeleportOutCamera", FNAME_Find), nullptr);
#endif
}

void AFortPlayerControllerZone::HookPossessionVTables()
{
#if WITH_SERVER_CODE
    // A client that takes a pawn (ClientRestart) answers with ServerAcknowledgePossession, and the server keeps the pawn
    // as the controller's AcknowledgedPawn. The server moves a pawn only for a client that has acknowledged it:
    // UCharacterMovementComponent::ServerMove_Implementation (CL-4204761 0x22A6890) asks
    // APlayerController::NotifyServerReceivedClientData (0x2608D30, slot 0x191), which is false while AcknowledgedPawn
    // isn't the pawn, and then drops the move. 5.0 overrides _Implementation in this class (slot 0x104, the exec
    // 0x28C2DD0's last call) and strips it to the shared empty function, which Battle Royale's controller inherits;
    // 4.5's classes (CL-4166199) all kept APlayerController's (0x23A51C0). No 5.0 pawn was ever acknowledged, so the
    // server never moved one: it hung where it spawned, and after the bus it kept skydiving where it left the bus
    // (#345, #346, #347, #348). Its abilities were refused (Jump and Sprint need a pawn on the ground, interacting one
    // that isn't skydiving), auto pickup saw a pawn that never moved, and the glider couldn't open.
    UFunction* const Function = APlayerController::FindStaticFunction_ServerAcknowledgePossession();
    const int32_t Index = Function ? Finder::ImplementationIndex(Function) : -1;
    UClass* const SuperClass = StaticClass()->GetSuperClass();
    UObject* const SuperDefault = SuperClass ? SuperClass->GetDefaultObject(false) : nullptr;
    void* const Super = SuperDefault && Index >= 0 ? (*reinterpret_cast<void***>(SuperDefault))[Index] : nullptr;
    if (!Super || Finder::IsStubFunction(reinterpret_cast<uintptr_t>(Super)))
    {
        Log::Error(L"Finder: APlayerController::ServerAcknowledgePossession_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && Finder::IsStubFunction(reinterpret_cast<uintptr_t>(VTable[Index])))
        {
            ServerAcknowledgePossession_ImplementationSuper = reinterpret_cast<decltype(ServerAcknowledgePossession_ImplementationSuper)>(Super);
            Hooks::SwapVTable(L"AFortPlayerControllerZone::ServerAcknowledgePossession_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerAcknowledgePossession_ImplementationHK));
        }
    }
#endif
}

void AFortPlayerControllerZone::ServerAcknowledgePossession_ImplementationHK(AFortPlayerControllerZone* This, APawn* P)
{
    This->ServerAcknowledgePossession_Implementation(P);
}

void AFortPlayerControllerZone::ServerAcknowledgePossession_Implementation(APawn* P)
{
    // Super: APlayerController's, which sets AcknowledgedPawn.
    ServerAcknowledgePossession_ImplementationSuper(this, P);
#if WITH_SERVER_CODE
    // "cheat disguise" and "cheat possessplayer" (#154): the client has taken the pawn they gave it.
    UFortCheatManager::RestoreKeptView(this, P);
#endif
}

AFortGameStateZone* AFortPlayerControllerZone::GetVoteGameState(const FUniqueNetIdRepl*& OutPlayerNetId) const
{
    UWorld* World = GetWorld();
    AFortGameStateZone* GameState = World ? Cast<AFortGameStateZone>(reinterpret_cast<UObject*>(World->GetGameState())) : nullptr;
    APlayerState* const Player = PlayerState;
    OutPlayerNetId = Player ? &Player->GetMember_UniqueId() : nullptr;
    return OutPlayerNetId ? GameState : nullptr;
}

void AFortPlayerControllerZone::ServerBeginGameplayVote_Implementation(EFortVoteType VoteType, float VoteDuration, float FailedVoteLockOutDuration, EFortVoteArbitratorType VoteArbitratorType)
{
    const FUniqueNetIdRepl* PlayerNetId = nullptr;
    if (AFortGameStateZone* GameState = GetVoteGameState(PlayerNetId))
    {
        GameState->ServerBeginGameplayVote(VoteType, *PlayerNetId, VoteDuration, FailedVoteLockOutDuration, VoteArbitratorType);
    }
}

void AFortPlayerControllerZone::ServerBeginGameplayVote_Implementation(EFortVoteType VoteType, const FFortVoteConfig& VoteConfig)
{
    const FUniqueNetIdRepl* PlayerNetId = nullptr;
    if (AFortGameStateZone* GameState = GetVoteGameState(PlayerNetId))
    {
        GameState->ServerBeginGameplayVote(VoteType, *PlayerNetId, VoteConfig);
    }
}

void AFortPlayerControllerZone::ServerSubmitGameplayVote_Implementation(EFortVoteType VoteType, int32_t VoteDecision)
{
    const FUniqueNetIdRepl* PlayerNetId = nullptr;
    if (AFortGameStateZone* GameState = GetVoteGameState(PlayerNetId))
    {
        GameState->ServerSubmitGameplayVote(VoteType, VoteDecision, *PlayerNetId);
    }
}

void AFortPlayerControllerZone::ServerEndGameplayVote_Implementation(EFortVoteType VoteType)
{
    const FUniqueNetIdRepl* PlayerNetId = nullptr;
    if (AFortGameStateZone* GameState = GetVoteGameState(PlayerNetId))
    {
        GameState->ServerEndGameplayVote(VoteType, *PlayerNetId);
    }
}

void AFortPlayerControllerZone::ServerBeginGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, float VoteDuration, float FailedVoteLockOutDuration, uint8_t VoteArbitratorType)
{
    This->ServerBeginGameplayVote_Implementation(EFortVoteType{ VoteType }, VoteDuration, FailedVoteLockOutDuration, EFortVoteArbitratorType{ VoteArbitratorType });
}

void AFortPlayerControllerZone::ServerBeginGameplayVoteConfig_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, const FFortVoteConfig* VoteConfig)
{
    if (VoteConfig)
    {
        This->ServerBeginGameplayVote_Implementation(EFortVoteType{ VoteType }, *VoteConfig);
    }
}

void AFortPlayerControllerZone::ServerSubmitGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType, int32_t VoteDecision)
{
    This->ServerSubmitGameplayVote_Implementation(EFortVoteType{ VoteType }, VoteDecision);
}

void AFortPlayerControllerZone::ServerEndGameplayVote_ImplementationHK(AFortPlayerControllerZone* This, uint8_t VoteType)
{
    This->ServerEndGameplayVote_Implementation(EFortVoteType{ VoteType });
}

void AFortPlayerControllerZone::HookGameplayVoteVTables()
{
#if WITH_SERVER_CODE
    // 5.10 CL-4240749: the RPCs' execs (0x1452710, 0x1453040, 0x1452940) call the _Implementations in slots 0x37B, 0x379
    // and 0x377 (0x11AF870, 0x11B0430, 0x11AFB90), which find the game state and the player's UniqueId and call the game
    // state's stripped functions. 5.0 (CL-4204761) has the same code (OnRep_GameplayVotes 0xE03A50, the same FVoteData)
    // but its UFortRuntimeOptions::bEnableMissionActivationVote is off, so no mission uses it; builds before 5.0 have
    // no votes. 6.00-6.20 (and, by their SDKs, 5.30-5.41) have the same _Implementations, Begin's taking the
    // FFortVoteConfig its exec copies (6.02
    // CL-4461277: execs 0x1538BC0, 0x15393A0, 0x1538DA0, slots 0x397, 0x395, 0x393: 0x1241620, 0x12421D0, 0x1241930,
    // each calling nullsub_3 for the game state's function).
    if (!FindStaticFunction_ServerBeginGameplayVote() || !AFortGameStateZone::AreGameplayVotesStripped())
    {
        return;
    }
    // The settings Begin takes: 5.0-5.21's VoteDuration, FailedVoteLockOutDuration and VoteArbitratorType, or 5.30's
    // FFortVoteConfig (which the first would read as floats).
    const bool bVoteConfig = FVoteData::HasVoteConfig();
    if (!bVoteConfig && !FVoteData::HasVoteSettings())
    {
        Log::Warning(L"Mission Control: this build's vote settings are neither in FVoteData nor an FFortVoteConfig; gameplay votes stay as the build has them");
        return;
    }
    struct FVoteRestore
    {
        UFunction* Function;
        const wchar_t* Name;
        void* Implementation;
    };
    void* const BeginImplementation = bVoteConfig ? reinterpret_cast<void*>(&ServerBeginGameplayVoteConfig_ImplementationHK) : reinterpret_cast<void*>(&ServerBeginGameplayVote_ImplementationHK);
    const FVoteRestore Restores[] = {
        { StaticFunction_ServerBeginGameplayVote(), L"AFortPlayerControllerZone::ServerBeginGameplayVote_Implementation", BeginImplementation },
        { StaticFunction_ServerSubmitGameplayVote(), L"AFortPlayerControllerZone::ServerSubmitGameplayVote_Implementation", reinterpret_cast<void*>(&ServerSubmitGameplayVote_ImplementationHK) },
        { StaticFunction_ServerEndGameplayVote(), L"AFortPlayerControllerZone::ServerEndGameplayVote_Implementation", reinterpret_cast<void*>(&ServerEndGameplayVote_ImplementationHK) },
    };
    UObject* const ZoneDefault = StaticClass()->GetDefaultObject(false);
    void** const ZoneVTable = ZoneDefault ? *reinterpret_cast<void***>(ZoneDefault) : nullptr;
    for (const FVoteRestore& Restore : Restores)
    {
        const int32_t Index = Restore.Function ? Finder::ImplementationIndex(Restore.Function) : -1;
        void* const Original = ZoneVTable && Index >= 0 ? ZoneVTable[Index] : nullptr;
        if (!Original)
        {
            Log::Error(L"Finder: {} slot not found", Restore.Name);
            return;
        }
        for (TObjectIterator<UClass> It; It; ++It)
        {
            UClass* Class = *It;
            UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
            void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
            if (VTable && VTable[Index] == Original)
            {
                Hooks::SwapVTable(Restore.Name, VTable, Index, Restore.Implementation);
            }
        }
    }
    const UFortRuntimeOptions* Options = UFortRuntimeOptions::Get();
    Log::Info(L"Mission Control: gameplay votes restored ({}; bEnableMissionActivationVote {})", bVoteConfig ? L"FFortVoteConfig" : L"FVoteData's settings",
        Options && UFortRuntimeOptions::HasMissionActivationVote() ? (Options->bEnableMissionActivationVote ? L"on" : L"off") : L"unknown");
#endif
}

namespace
{
    // An element of the controller's map of applied gadgets with tracked attributes (TMap<const
    // UFortGadgetItemDefinition*, FGuid>'s sparse array): the gadget, its item, then the hash's links.
    struct FGadgetTrackedAttributeItem
    {
        const UFortGadgetItemDefinition* Gadget;
        FGuid ItemGuid;
        int32_t HashNextId;
        int32_t HashIndex;
    };
    static_assert(sizeof(FGadgetTrackedAttributeItem) == 0x20);

    // 7.30+ (Natives::AFortPlayerControllerZone_bGadgetTrackedAttributeItemGuidSet): an element of the controller's
    // TSet<FGuid> of the applied gadgets' items, the gadget being the item's definition (7.30 Add 0x142BA00).
    struct FGadgetTrackedAttributeItemGuid
    {
        FGuid ItemGuid;
        int32_t HashNextId;
        int32_t HashIndex;
    };
    static_assert(sizeof(FGadgetTrackedAttributeItemGuid) == 0x18);

    // The attribute's base value on the ability system (what ApplyGadgetItemAttributeSet sets back, through
    // SetNumericAttributeBase): the FGameplayAttributeData its property holds in the ability system's set of the
    // attribute's class.
    bool GetNumericAttributeBase(const UAbilitySystemComponent* AbilitySystemComponent, const FGameplayAttribute& Attribute, float& OutValue)
    {
        UProperty* Property = Attribute.Attribute;
        UClass* AttributeOwner = reinterpret_cast<UClass*>(Attribute.AttributeOwner);
        if (!Property || !AttributeOwner || !Property->IsA(UStructProperty::StaticClass()))
        {
            return false;
        }
        const TArray<UAttributeSet*>& Sets = AbilitySystemComponent->SpawnedAttributes;
        for (int32_t Index = 0; Index < Sets.Num(); ++Index)
        {
            if (Sets[Index] && Sets[Index]->IsA(AttributeOwner))
            {
                OutValue = Property->ContainerPtrToValuePtr<FGameplayAttributeData>(Sets[Index])->GetBaseValue();
                return true;
            }
        }
        return false;
    }

    // bDestroyGadgetWhenTrackedAttributesIsZero: every tracked attribute at 0. 8.40's jetpack (AGID_BoostJumpPack) tracks
    // Fuel and ReserveFuel: Fuel at 0 is its overheat, which UFortGameplayAbility_JumpBoostPack's tick (CL-6005771 0xCDF650)
    // refills from ReserveFuel after FuelRegenDelay; only with the reserve gone too does the pack break off.
    bool AreTrackedAttributesZero(const TArray<float>& Values, int32_t NumTrackedAttributes)
    {
        if (NumTrackedAttributes <= 0 || Values.Num() < NumTrackedAttributes)
        {
            return false;
        }
        for (int32_t Index = 0; Index < NumTrackedAttributes; ++Index)
        {
            if (Values[Index] > 0.0f)
            {
                return false;
            }
        }
        return true;
    }
}

void AFortPlayerControllerZone::OnGadgetTrackedAttributeChangedHK(AFortPlayerControllerZone* This, float NewValue, const void* ModCallbackData)
{
    This->OnGadgetTrackedAttributeChanged(NewValue, ModCallbackData);
}

void AFortPlayerControllerZone::OnGadgetTrackedAttributeChanged(float NewValue, const void* ModCallbackData)
{
#if WITH_SERVER_CODE
    // The delegate gives only the new value (and, for an effect's execution, the modifier's data): every applied gadget's
    // tracked attributes are read again from the ability system. Which item keeps them is the map's (the gadget's latest
    // item), as ApplyGadgetItemAttributeSet records it.
    const ptrdiff_t ItemInstanceIdsOffset = Natives::AFortPlayerControllerZone_GadgetTrackedAttributeItemInstanceIds;
    AFortInventory* Inventory = Role == ROLE_Authority ? static_cast<AFortInventory*>(WorldInventory) : nullptr;
    AFortPawn* Pawn = MyFortPawn;
    UAbilitySystemComponent* AbilitySystemComponent = Pawn ? UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(reinterpret_cast<AActor*>(Pawn)) : nullptr;
    if (ItemInstanceIdsOffset < 0 || !Inventory || !AbilitySystemComponent)
    {
        return;
    }

    // 3.5-6.10 keep the one applied gadget's item (GadgetTrackedAttributeItemInstanceId); 6.20-7.20 keep a map of each
    // gadget to its item's guid; 7.30+ keep a set of the items' guids, whose gadget is the item's definition
    // (Finder::GadgetTrackedAttributeBind tells which). The containers are a sparse array read element by element.
    const uint8_t* const Container = reinterpret_cast<const uint8_t*>(this) + ItemInstanceIdsOffset;
    const bool bSingleItemGuid = HasGadgetTrackedAttributeItemInstanceId();
    const bool bItemGuidSet = Natives::AFortPlayerControllerZone_bGadgetTrackedAttributeItemGuidSet;
    const int32_t NumElements = bSingleItemGuid ? 1
        : bItemGuidSet ? reinterpret_cast<const TArray<FGadgetTrackedAttributeItemGuid>*>(Container)->Num()
        : reinterpret_cast<const TArray<FGadgetTrackedAttributeItem>*>(Container)->Num();
    struct FUsedUpGadget
    {
        FGuid ItemGuid;
        const UFortGadgetItemDefinition* Gadget;
    };
    TArray<FUsedUpGadget> UsedUp;
    for (int32_t Index = 0; Index < NumElements; ++Index)
    {
        // A free slot of the sparse array holds its free list's indices where the gadget or the guid would be, which
        // never is an item's definition or an item's guid: only the slots whose item the inventory holds (with that
        // definition, for the map) are read further.
        FGuid ItemGuid;
        const UFortGadgetItemDefinition* Gadget = nullptr;
        if (bSingleItemGuid)
        {
            ItemGuid = *reinterpret_cast<const FGuid*>(Container);
        }
        else if (bItemGuidSet)
        {
            ItemGuid = (*reinterpret_cast<const TArray<FGadgetTrackedAttributeItemGuid>*>(Container))[Index].ItemGuid;
        }
        else
        {
            const FGadgetTrackedAttributeItem& Applied = (*reinterpret_cast<const TArray<FGadgetTrackedAttributeItem>*>(Container))[Index];
            ItemGuid = Applied.ItemGuid;
            Gadget = Applied.Gadget;
        }
        UFortWorldItem* Item = Inventory->FindWorldItem(ItemGuid);
        FFortItemEntry* Entry = Item ? &static_cast<FFortItemEntry&>(Item->ItemEntry) : nullptr;
        if (Entry && (bSingleItemGuid || bItemGuidSet))
        {
            Gadget = Cast<UFortGadgetItemDefinition>(static_cast<UFortItemDefinition*>(Entry->ItemDefinition));
        }
        if (!Entry || !Gadget || static_cast<UFortItemDefinition*>(Entry->ItemDefinition) != static_cast<const UFortItemDefinition*>(Gadget))
        {
            continue;
        }

        const TArray<FGameplayAttribute>& TrackedAttributes = Gadget->TrackedAttributes;
        TArray<float>& Values = Entry->GenericAttributeValues;
        if (Values.Num() < TrackedAttributes.Num())
        {
            Values.AddZeroed(TrackedAttributes.Num() - Values.Num());
        }
        for (int32_t Attribute = 0; Attribute < TrackedAttributes.Num(); ++Attribute)
        {
            float Value = 0.0f;
            if (GetNumericAttributeBase(AbilitySystemComponent, TrackedAttributes[Attribute], Value))
            {
                Values[Attribute] = Value;
            }
        }
        Entry->SetStateValue(EFortItemEntryState::GetGenericAttributeValueSet(), 1);

        static bool bLoggedOnce = false;
        if (!bLoggedOnce)
        {
            bLoggedOnce = true;
            Log::Info(L"Gadget: {}'s {} keeps its tracked attributes ({} attributes, the first now {:.0f})", GetName().ToStdString(), Gadget->GetName().ToStdString(),
                TrackedAttributes.Num(), Values.Num() > 0 ? Values[0] : 0.0f);
        }
        if (Gadget->bDestroyGadgetWhenTrackedAttributesIsZero && AreTrackedAttributesZero(Values, TrackedAttributes.Num()))
        {
            UsedUp.Add({ static_cast<FGuid>(Entry->ItemGuid), Gadget });
        }
    }
    for (int32_t Index = 0; Index < UsedUp.Num(); ++Index)
    {
        DestroyGadgetTrackedAttributeItem(UsedUp[Index].ItemGuid, UsedUp[Index].Gadget);
    }
#endif
}

int32_t AFortPlayerControllerZone::ApplyInventoryGadgetData()
{
    int32_t Applied = 0;
#if WITH_SERVER_CODE
    // 9.00 (CL-6337466) Playground's and Food Fight's big team glider (Athena_Glider_Item_BigTeamMode, CoreV2 #672) is
    // given at BusFlying by the restored give-items mutator, to players still in the aircraft. Its definition's
    // OnItemInstanceAdded (slot 0x73, 0x1508C00) calls ApplyGadgetData (0x14F62B0), whose ability set
    // (ApplyGadgetItemAbilitySet 0x17D5490) and attribute set (ApplyGadgetItemAttributeSet 0x17D5620, the glider's
    // CurrentCharges from AttributeInitKey GliderPack.BigTeamMode) both return without the controller's pawn. Nothing in
    // the build applies them later (both have ApplyGadgetData as their only caller), so the glider had no charges and no
    // ability: on its death the server logged "RemoveGadgetItemAbilitySet - Gadget Item [Athena_Glider_Item_BigTeamMode]
    // has an ability set, but applied ability set handle was invalid!". 🟡 Where Epic's server applied it once the pawn
    // came isn't in the build; Core applies it as the pawn is possessed. Only where Core restores the give-items
    // mutators (8.30 to 9.00), the only way the server gives a gadget to a player without a pawn.
    const ptrdiff_t ItemInstanceIdsOffset = Natives::AFortPlayerControllerZone_GadgetTrackedAttributeItemInstanceIds;
    AFortInventory* Inventory = Role == ROLE_Authority && MyFortPawn ? static_cast<AFortInventory*>(WorldInventory) : nullptr;
    static UClass* const InventoryOwnerInterfaceClass = FindObject<UClass>(nullptr, L"/Script/FortniteGame.FortInventoryOwnerInterface");
    if (!Inventory || !Natives::UFortGadgetItemDefinition_ApplyGadgetData || ItemInstanceIdsOffset < 0 || !Natives::AFortPlayerControllerZone_bGadgetTrackedAttributeItemGuidSet
        || !InventoryOwnerInterfaceClass || !Natives::UObject_GetInterfaceAddress || !AFortAthenaMutator_GiveItemsAtGamePhaseStep::AreNativesRestored())
    {
        return 0;
    }
    void* const InventoryOwner = Natives::UObject_GetInterfaceAddress(this, InventoryOwnerInterfaceClass);
    if (!InventoryOwner)
    {
        return 0;
    }
    // The applied items, a sparse array read element by element (a free slot never holds an item's guid).
    const TArray<FGadgetTrackedAttributeItemGuid>& AppliedItems = *reinterpret_cast<const TArray<FGadgetTrackedAttributeItemGuid>*>(reinterpret_cast<const uint8_t*>(this) + ItemInstanceIdsOffset);
    const auto IsApplied = [&AppliedItems](const FGuid& ItemGuid)
    {
        for (int32_t Index = 0; Index < AppliedItems.Num(); ++Index)
        {
            if (AppliedItems[Index].ItemGuid == ItemGuid)
            {
                return true;
            }
        }
        return false;
    };

    TArray<UFortWorldItem*> Pending;
    FFortItemList& ItemList = Inventory->Inventory;
    TArray<UFortWorldItem*>& Instances = ItemList.ItemInstances;
    for (int32_t Index = 0; Index < Instances.Num(); ++Index)
    {
        UFortWorldItem* Item = Instances[Index];
        FFortItemEntry* Entry = Item ? &static_cast<FFortItemEntry&>(Item->ItemEntry) : nullptr;
        UFortGadgetItemDefinition* Gadget = Entry ? Cast<UFortGadgetItemDefinition>(static_cast<UFortItemDefinition*>(Entry->ItemDefinition)) : nullptr;
        if (Gadget && Gadget->TrackedAttributes.Num() > 0 && !IsApplied(static_cast<FGuid>(Entry->ItemGuid)))
        {
            Pending.Add(Item);
        }
    }
    for (int32_t Index = 0; Index < Pending.Num(); ++Index)
    {
        UFortWorldItem* Item = Pending[Index];
        UObject* Gadget = static_cast<UFortItemDefinition*>(static_cast<FFortItemEntry&>(Item->ItemEntry).ItemDefinition);
        Natives::UFortGadgetItemDefinition_ApplyGadgetData(Gadget, InventoryOwner, Item, false);
        ++Applied;
        Log::Info(L"Gadget: {}'s {} was given without a pawn; its gadget data goes on {} now", GetName().ToStdString(), Gadget->GetName().ToStdString(),
            reinterpret_cast<UObject*>(static_cast<AFortPawn*>(MyFortPawn))->GetName().ToStdString());
    }
#endif
    return Applied;
}

void AFortPlayerControllerZone::DestroyGadgetTrackedAttributeItem(const FGuid& ItemGuid, const UFortGadgetItemDefinition* GadgetItemDefinition)
{
#if WITH_SERVER_CODE
    // Core's: on the next tick (the engine's FTimerManager). The change comes in the middle of the ability system's
    // attribute update, and the item's removal takes the gadget's ability set and this delegate's binding away
    // (UFortGadgetItemDefinition::RemoveGadgetData).
    UWorld* World = GetWorld();
    if (!World)
    {
        return;
    }
    const TWeakObjectPtr<AFortPlayerControllerZone> WeakThis(this);
    World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis, ItemGuid, GadgetItemDefinition]
    {
        AFortPlayerControllerZone* Controller = WeakThis.Get();
        AFortInventory* Inventory = Controller ? static_cast<AFortInventory*>(Controller->WorldInventory) : nullptr;
        UFortWorldItem* Item = Inventory ? Inventory->FindWorldItem(ItemGuid) : nullptr;
        FFortItemEntry* Entry = Item ? &static_cast<FFortItemEntry&>(Item->ItemEntry) : nullptr;
        // Gone already (a second change at 0 in the same tick).
        if (!Entry || static_cast<UFortItemDefinition*>(Entry->ItemDefinition) != static_cast<const UFortItemDefinition*>(GadgetItemDefinition))
        {
            return;
        }
        // Still used up: nothing gave it back in between.
        if (!AreTrackedAttributesZero(Entry->GenericAttributeValues, GadgetItemDefinition->TrackedAttributes.Num()) || !Controller->RemoveInventoryItem(ItemGuid, Entry->Count, true, true))
        {
            return;
        }
        AFortPlayerStateZone* FortPlayerState = Cast<AFortPlayerStateZone>(reinterpret_cast<UObject*>(static_cast<APlayerState*>(Controller->PlayerState)));
        const bool bEffects = FortPlayerState && FortPlayerState->MulticastTriggerOnGadgetTrackedAttributeDestroyedFX(GadgetItemDefinition);
        Log::Info(L"Gadget: {}'s {} is used up and goes{}", Controller->GetName().ToStdString(), GadgetItemDefinition->GetName().ToStdString(), bEffects ? L"" : L" (no destroy effects)");
    }));
#endif
}

void AFortPlayerControllerZone::HookGadgetTrackedAttributeDelegate()
{
    // 4.0+ (FFortItemEntry::GenericAttributeValues, the gadget's TrackedAttributes): restored only where the build binds
    // the shared empty function (Finder::GadgetTrackedAttributeBind says which).
    if (!UFortGadgetItemDefinition::HasTrackedAttributes() || !FFortItemEntry::HasGenericAttributeValues()
        || EFortItemEntryState::GetGenericAttributeValueSet() == Reflection::InvalidEnumValue)
    {
        return;
    }
    const bool bSingleItemGuid = HasGadgetTrackedAttributeItemInstanceId();
    const Finder::FGadgetTrackedAttributeBind Bind = Finder::GadgetTrackedAttributeBind(UFortGadgetItemDefinition::TrackedAttributesOffset(), bSingleItemGuid);
    const ptrdiff_t ItemInstanceIds = bSingleItemGuid ? GadgetTrackedAttributeItemInstanceIdOffset() : Bind.ItemInstanceIds;
    if (!Bind.Lea || ItemInstanceIds < 0)
    {
        return;
    }
    Natives::AFortPlayerControllerZone_GadgetTrackedAttributeItemInstanceIds = ItemInstanceIds;
    Natives::AFortPlayerControllerZone_bGadgetTrackedAttributeItemGuidSet = Bind.bItemGuidSet;
    Hooks::RedirectAddress(L"AFortPlayerControllerZone::OnGadgetTrackedAttributeChanged (ApplyGadgetItemAttributeSet's binding)", Bind.Lea, reinterpret_cast<void*>(&OnGadgetTrackedAttributeChangedHK));
}

void AFortPlayerControllerZone::HookVTables()
{
    HookPossessionVTables();
    HookGameplayVoteVTables();
    HookGadgetTrackedAttributeDelegate();

    // 8.00 (CL-5203069) moves the RPC to AFortPlayerControllerPvE, which restores it there.
    UFunction* TeleportCameraFunction = FindStaticFunction_ServerTriggerSetupTeleportCameraEvent();
    if (!TeleportCameraFunction)
    {
        return;
    }
    const int32_t Index = Finder::ImplementationIndex(TeleportCameraFunction);
    if (Index < 0)
    {
        Log::Error(L"Finder: AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation slot not found");
        return;
    }

    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_Implementation", VTable, Index, reinterpret_cast<void*>(&ServerTriggerSetupTeleportCameraEvent_ImplementationHK));
        }
    }
}

void AFortPlayerControllerZone::ServerTriggerSetupTeleportCameraEvent_ImplementationHK(AFortPlayerControllerZone* This)
{
    This->ServerTriggerSetupTeleportCameraEvent_Implementation();
}
