#include "pch.h"
#include "Abilities/FortGameplayAbilityAthena_PeriodicItemGrant.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/PlayerState.h"
#include "FortKismetLibrary.h"
#include "Items/FortInventory.h"
#include "Items/FortItem.h"
#include "Items/FortItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Player/FortPlayerController.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/ScriptInterface.h"
#include "UObject/UObjectIterator.h"

#include <algorithm>

namespace
{
    // The curve rows ItemsToGrant uses are playlist game data (AthenaCompositeGD_Barrier's Lava.WoodTick.Time: 3 at
    // time 0, 0 from time 1 on), read at level 0 as the game's other playlist values are.
    constexpr float GrantCurveLevel = 0.0f;

    bool bLoggedFirstStart = false;

    // 8.40 strips both natives: they share one exec (0x9C5000, folded with UNiagaraScript::execOnCompilationComplete),
    // which steps over the parameters and tail-jumps into an empty function (0x310BA0, `ret 0`). Two natives with
    // bodies of their own never share an exec, and the build's empty exec is shared by many UFunctions (as
    // AFortMinigame's stripped execs are). The exec isn't decoded: on a packed exe its code may have moved.
    int32_t CountExecSharers(FNativeFuncPtr Exec)
    {
        int32_t Sharing = 0;
        for (TObjectIterator<UFunction> It; It && Exec; ++It)
        {
            Sharing += (*It)->GetNativeFunc() == Exec ? 1 : 0;
        }
        return Sharing;
    }
}

FScriptMapLayout UFortGameplayAbilityAthena_PeriodicItemGrant::GetItemsToGrantLayout() const
{
    UScriptStruct* KeyStruct = FActiveItemGrantInfo::StaticStruct();
    UScriptStruct* ValueStruct = FScalableFloat::StaticStruct();
    if (!KeyStruct || !ValueStruct)
    {
        return FScriptMapLayout();
    }
    return FScriptMap::GetScriptLayout(KeyStruct->GetStructureSize(), KeyStruct->GetMinAlignment(), ValueStruct->GetStructureSize(), ValueStruct->GetMinAlignment());
}

void UFortGameplayAbilityAthena_PeriodicItemGrant::StartItemAwardTimers()
{
#if WITH_SERVER_CODE
    // Core's: an ability activated again doesn't keep the last activation's timers running beside the new ones.
    StopItemAwardTimers();

    AFortPawn* Pawn = GetActivatingPawn();
    AFortPlayerController* Controller = Pawn ? Cast<AFortPlayerController>(reinterpret_cast<UObject*>(static_cast<AController*>(Pawn->Controller))) : nullptr;
    OwnerControllerCachedValue = Controller;
    UWorld* World = GetWorld();
    const FScriptMapLayout Layout = GetItemsToGrantLayout();
    if (!Controller || !World || Layout.ElementSize <= 0)
    {
        return;
    }

    const FScriptMap& Items = ItemsToGrant;
    TArray<FTimerHandle>& Timers = ActiveTimers;
    for (int32_t Index = 0; Index < Items.GetMaxIndex(); ++Index)
    {
        if (!Items.IsValidIndex(Index))
        {
            continue;
        }
        const FScalableFloat& Interval = *reinterpret_cast<const FScalableFloat*>(Items.GetData(Index, Layout) + Layout.ValueOffset);
        const float Rate = Interval.GetValueAtLevel(GrantCurveLevel);
        if (Rate <= 0.0f)
        {
            continue;
        }
        FTimerHandle Handle;
        World->GetTimerManager().SetTimer(Handle, FTimerDelegate::CreateUObject(this, &UFortGameplayAbilityAthena_PeriodicItemGrant::AwardItem, Index), Rate, true);
        Timers.Add(Handle);
    }

    if (!bLoggedFirstStart)
    {
        bLoggedFirstStart = true;
        const APlayerState* PlayerState = static_cast<APlayerState*>(Controller->PlayerState);
        Log::Info(L"Periodic item grant: {} starts {} item timers for {}", GetName().ToStdString(), Timers.Num(),
            PlayerState ? PlayerState->GetPlayerName().ToStdString() : Controller->GetName().ToStdString());
    }
#endif
}

void UFortGameplayAbilityAthena_PeriodicItemGrant::StopItemAwardTimers()
{
#if WITH_SERVER_CODE
    TArray<FTimerHandle>& Timers = ActiveTimers;
    UWorld* World = GetWorld();
    for (int32_t Index = 0; World && Index < Timers.Num(); ++Index)
    {
        World->GetTimerManager().ClearTimer(Timers[Index]);
    }
    Timers.Empty();
#endif
}

void UFortGameplayAbilityAthena_PeriodicItemGrant::AwardItem(int32_t Index)
{
#if WITH_SERVER_CODE
    AFortPlayerController* Controller = OwnerControllerCachedValue;
    AFortInventory* Inventory = Controller ? static_cast<AFortInventory*>(Controller->WorldInventory) : nullptr;
    const FScriptMap& Items = ItemsToGrant;
    const FScriptMapLayout Layout = GetItemsToGrantLayout();
    if (!Inventory || Layout.ElementSize <= 0 || !Items.IsValidIndex(Index))
    {
        return;
    }
    const FActiveItemGrantInfo& Info = *reinterpret_cast<const FActiveItemGrantInfo*>(Items.GetData(Index, Layout));
    UFortWorldItemDefinition* Item = Info.Item;
    if (!Item)
    {
        return;
    }

    // MaxAmount caps what the owner holds (Lava.MatCap.Check, 999): a timer tops up to it and gives nothing past it.
    int32_t Held = 0;
    TArray<UFortWorldItem*>& Instances = static_cast<FFortItemList&>(Inventory->Inventory).ItemInstances;
    for (int32_t ItemIndex = 0; ItemIndex < Instances.Num(); ++ItemIndex)
    {
        const FFortItemEntry& Entry = Instances[ItemIndex]->ItemEntry;
        if (static_cast<UFortItemDefinition*>(Entry.ItemDefinition) == Item)
        {
            Held += Entry.Count;
        }
    }
    const int32_t MaxAmount = static_cast<int32_t>(Info.MaxAmount.GetValueAtLevel(GrantCurveLevel));
    const int32_t Amount = std::min(static_cast<int32_t>(Info.AmountToGive.GetValueAtLevel(GrantCurveLevel)), MaxAmount - Held);
    if (Amount > 0)
    {
        UFortKismetLibrary::GiveItemToInventoryOwner(TScriptInterface<IFortInventoryOwnerInterface>(Controller, nullptr), Item, Amount, false);
    }
#endif
}

DEFINE_FUNCTION(UFortGameplayAbilityAthena_PeriodicItemGrant::execStartItemAwardTimers)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortGameplayAbilityAthena_PeriodicItemGrant*>(Context)->StartItemAwardTimers();
    P_NATIVE_END;
}

DEFINE_FUNCTION(UFortGameplayAbilityAthena_PeriodicItemGrant::execStopItemAwardTimers)
{
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortGameplayAbilityAthena_PeriodicItemGrant*>(Context)->StopItemAwardTimers();
    P_NATIVE_END;
}

void UFortGameplayAbilityAthena_PeriodicItemGrant::HookNatives()
{
    // 8.40 added the class; older builds have nothing to restore.
    UFunction* Start = StaticClass() ? FindStaticFunction_StartItemAwardTimers() : nullptr;
    UFunction* Stop = Start ? FindStaticFunction_StopItemAwardTimers() : nullptr;
    if (!Start || !Stop)
    {
        return;
    }
    const FNativeFuncPtr Exec = Start->GetNativeFunc();
    const int32_t Sharing = Exec == Stop->GetNativeFunc() ? CountExecSharers(Exec) : 0;
    if (Sharing <= 16)
    {
        Log::Info(L"Finder: UFortGameplayAbilityAthena_PeriodicItemGrant's StartItemAwardTimers and StopItemAwardTimers have the build's own body; left as they are");
        return;
    }
    if (!Member_ItemsToGrant.IsAvailable() || !Member_ActiveTimers.IsAvailable() || !Member_OwnerControllerCachedValue.IsAvailable() || !FActiveItemGrantInfo::StaticStruct()
        || !FScalableFloat::StaticStruct())
    {
        Log::Error(L"Finder: UFortGameplayAbilityAthena_PeriodicItemGrant's item timers are stripped but its members aren't all reflected (ItemsToGrant {}, ActiveTimers {}, OwnerControllerCachedValue {}); not restored",
            Member_ItemsToGrant.IsAvailable(), Member_ActiveTimers.IsAvailable(), Member_OwnerControllerCachedValue.IsAvailable());
        return;
    }
    Start->SetNativeFunc(&execStartItemAwardTimers);
    Stop->SetNativeFunc(&execStopItemAwardTimers);
    Log::Info(L"Finder: UFortGameplayAbilityAthena_PeriodicItemGrant::StartItemAwardTimers and StopItemAwardTimers are stripped (exec 0x{:X}, shared by {} UFunctions), restored (natives)", reinterpret_cast<uintptr_t>(Exec) - Finder::ImageBase(), Sharing);
}
