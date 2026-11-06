#include "pch.h"
#include "Player/FortPlayerController.h"
#include "Abilities/GameplayAbility.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Finder.h"
#include "GameplayTagContainer.h"
#include "Hooks.h"
#include "Items/FortPersonalVehicleItem.h"
#include "Items/FortPersonalVehicleItemDefinition.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Pawns/FortPlayerPawn.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

#include <vector>

// Save the World's Hoverboard (3.0, CL-3901517; #224). The Hoverboard is an account item in profile0,
// PersonalVehicle:VID_Hoverboard (UFortPersonalVehicleItemDefinition: PersonalVehicleAbility GA_Hoverboard, MountTime 3).
// The client holds the use key for MountTime (its own timer, 0xE49090 -> 0xE48FE0) and then sends
// TogglePersonalVehicle(true); pressing it again while riding sends TogglePersonalVehicle(false) (0xE61330). Both only if
// the client's own MainMcpProfile has the item (IsPersonalVehicleAvailable).
//
// 3.0 strips the RPC's body: AFortPlayerController's slot 0x21A (and AFortPlayerControllerGameplay's and
// AFortPlayerControllerZone's) holds the shared empty function, and no code left in the build reads PersonalVehicleAbility
// or calls GetCurrentlySelectedPersonalVehicle on the server's side. So nothing was ever ridden.
//
// Verified in 3.0: the RPC is Net|NetServer|NetValidate, its _Validate is "return true"; GA_Hoverboard is
// ServerInitiated and InstancedPerActor with bRetriggerInstancedAbility, and its graph mounts when activated from state 0
// (attachment, GE_Hoverboard, the Running cue) and dismounts when activated again while riding (state 2); damage or a
// weapon equip makes it dismount itself. Its ActivationOwnedTags (Gameplay.Action.Player.PersonalVehicle) make the pawn's
// own tag handler (0xDF4E10 -> 0xE13C70) switch it to the PersonalVehicle movement style.
// Inferred (the body is gone): that the server gives the ability from the selected vehicle and activates it, and that
// turning it off ends it. 21.40 keeps the spec handle on the pawn (AFortPlayerPawn::PersonalVehicleAbilitySpecHandle);
// Core finds the ride's spec by its ability instead, so the pawn's layout isn't needed. Each ride gets its own spec (see
// below, #302): the graph's other way off, activating it again while riding (state 2), isn't used, as retriggering a spec
// that is removed after its activation would clear it under the activation.

UFortPersonalVehicleItem* AFortPlayerController::GetCurrentlySelectedPersonalVehicle() const
{
    return Natives::AFortPlayerController_GetCurrentlySelectedPersonalVehicle ? Natives::AFortPlayerController_GetCurrentlySelectedPersonalVehicle(this) : nullptr;
}

void AFortPlayerController::TogglePersonalVehicle_Implementation(bool bOn)
{
#if WITH_SERVER_CODE
    AFortPlayerPawn* PlayerPawn = Cast<AFortPlayerPawn>(MyFortPawn);
    if (!PlayerPawn || PlayerPawn->IsUsingPersonalVehicle() == bOn)
    {
        return;
    }

    // The client asks CanPerformAction(Gameplay.Action.Player.PersonalVehicle) before it sends the RPC (0xE48FE0); the
    // server asks again for getting on.
    if (bOn && Natives::AFortPlayerController_CanPerformActionIndex >= 0)
    {
        const FGameplayTag PersonalVehicleTag{ FName(L"Gameplay.Action.Player.PersonalVehicle") };
        if (!CallVirtual<bool>(this, Natives::AFortPlayerController_CanPerformActionIndex, &PersonalVehicleTag))
        {
            return;
        }
    }

    UFortPersonalVehicleItem* VehicleItem = GetCurrentlySelectedPersonalVehicle();
    UFortPersonalVehicleItemDefinition* VehicleDefinition = VehicleItem ? Cast<UFortPersonalVehicleItemDefinition>(reinterpret_cast<UObject*>(VehicleItem->GetItemDefinitionBP())) : nullptr;
    UClass* AbilityClass = VehicleDefinition ? VehicleDefinition->PersonalVehicleAbility.LoadSynchronous() : nullptr;
    if (!AbilityClass || !AbilityClass->IsChildOf(UGameplayAbility::StaticClass()))
    {
        Log::Warning(L"PersonalVehicle: {} asked for its personal vehicle, but its profile has none with an ability ({})", GetName().ToStdString(),
            VehicleItem ? VehicleItem->GetName().ToStdString() : std::wstring(L"no item"));
        return;
    }

    UAbilitySystemComponent* AbilitySystem = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(PlayerPawn);
    if (!AbilitySystem)
    {
        return;
    }

    // The ride going on: the vehicle ability's spec with an active instance.
    UObject* const AbilityDefault = AbilityClass->GetDefaultObject();
    UGameplayAbility* RidingInstance = nullptr;
    TStructArray<FGameplayAbilitySpec>& Specs = AbilitySystem->ActivatableAbilities.Items;
    for (int32_t Index = 0; Index < Specs.Num() && !RidingInstance; ++Index)
    {
        FGameplayAbilitySpec& Spec = Specs[Index];
        UGameplayAbility* const SpecAbility = Spec.Ability;
        const uint8_t ActiveCount = Spec.ActiveCount;
        if (SpecAbility != AbilityDefault || ActiveCount == 0)
        {
            continue;
        }
        TArray<UGameplayAbility*>& ReplicatedInstances = Spec.ReplicatedInstances;
        TArray<UGameplayAbility*>& NonReplicatedInstances = Spec.NonReplicatedInstances;
        for (TArray<UGameplayAbility*>* Instances : { &ReplicatedInstances, &NonReplicatedInstances })
        {
            for (UGameplayAbility* Instance : *Instances)
            {
                if (Instance && !RidingInstance)
                {
                    RidingInstance = Instance;
                }
            }
        }
    }

    if (!bOn)
    {
        // Off: cancel the ride. K2_OnEndAbility(bWasCancelled) cleans up (effect, attachment, delegates) and puts the graph
        // back in its idle state, on the server and on the rider's client; the ride's spec goes with the activation.
        if (RidingInstance)
        {
            RidingInstance->K2_CancelAbility();
            Log::Info(L"PersonalVehicle: {} dismounts {}", GetName().ToStdString(), AbilityClass->GetName().ToStdString());
        }
        return;
    }
    if (RidingInstance)
    {
        return;
    }

    // On: a new spec for each ride, FGameplayAbilitySpec(the ability's default object, level 1, no input, the vehicle item),
    // given and activated once, so it is removed when the ride ends (#302). A spec kept between rides keeps its instance and
    // the graph's state in it: damage or a weapon equip (OnPawnDamaged, OnEquippedWeapon -> ForceDismount) ends the ride on
    // the server only, the rider's client then ends its instance without bWasCancelled and leaves it in the riding state,
    // and from then on every activation the server starts is ended at once by the client's instance, which dismounts
    // instead. A new instance starts idle on both.
    UScriptStruct* SpecStruct = FGameplayAbilitySpec::StaticStruct();
    std::vector<uint8_t> SpecStorage(SpecStruct->GetStructureSize(), 0);
    SpecStruct->InitializeStruct(SpecStorage.data());
    FGameplayAbilitySpec& Spec = *reinterpret_cast<FGameplayAbilitySpec*>(SpecStorage.data());
    FGameplayAbilitySpecHandle NewHandle;
    NewHandle.GenerateNewHandle();
    Spec.Handle = NewHandle;
    Spec.Ability = static_cast<UGameplayAbility*>(AbilityDefault);
    Spec.Level = 1;
    Spec.InputID = INDEX_NONE;
    Spec.SourceObject = VehicleItem;
    const FGameplayAbilitySpecHandle Handle = AbilitySystem->GiveAbilityAndActivateOnce(Spec);
    SpecStruct->DestroyStruct(SpecStorage.data());
    Log::Info(L"PersonalVehicle: {} mounts {} ({}){}", GetName().ToStdString(), AbilityClass->GetName().ToStdString(), VehicleItem->GetName().ToStdString(),
        Handle.IsValid() ? L"" : L" (activation failed)");
#endif
}

void AFortPlayerController::HookPersonalVehicleVTables()
{
    // Builds before 3.0 have no personal vehicles.
    UFunction* TogglePersonalVehicle = FindStaticFunction_TogglePersonalVehicle();
    UFunction* IsPersonalVehicleAvailable = FindStaticFunction_IsPersonalVehicleAvailable();
    if (!TogglePersonalVehicle || !IsPersonalVehicleAvailable)
    {
        return;
    }

    const int32_t Index = Finder::ImplementationIndex(TogglePersonalVehicle);
    UObject* PlayerControllerDefault = StaticClass()->GetDefaultObject(false);
    void** PlayerControllerVTable = PlayerControllerDefault ? *reinterpret_cast<void***>(PlayerControllerDefault) : nullptr;
    if (Index < 0 || !PlayerControllerVTable)
    {
        Log::Error(L"Finder: AFortPlayerController::TogglePersonalVehicle_Implementation slot not found");
        return;
    }
    if (PlayerControllerVTable[Index] != Natives::SharedEmptyFunction)
    {
        Log::Info(L"Finder: AFortPlayerController::TogglePersonalVehicle_Implementation (slot 0x{:X}) is native in this build", Index);
        return;
    }
    if (!Finder::ResolvePersonalVehicleLive(IsPersonalVehicleAvailable, Member_MainMcpProfile.GetOffsetChecked())
        || !Finder::ResolveEmoteLive(MyFortPawnOffset(), AFortPlayerPawn::StasisModeOffset())
        || !Natives::UAbilitySystemComponent_GiveAbilityAndActivateOnce
        || !UGameplayAbility::FindStaticFunction_K2_CancelAbility())
    {
        // ResolveEmoteLive also finds GiveAbilityAndActivateOnce, the spec handle counter and CanPerformAction.
        Log::Error(L"PersonalVehicle: this build strips TogglePersonalVehicle_Implementation and its helpers were not found, so nobody can ride a hoverboard");
        return;
    }

    int32_t Swapped = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Natives::SharedEmptyFunction)
        {
            Hooks::SwapVTable(L"AFortPlayerController::TogglePersonalVehicle_Implementation", VTable, Index, reinterpret_cast<void*>(&TogglePersonalVehicle_ImplementationHK));
            ++Swapped;
        }
    }
    Log::Info(L"Finder: AFortPlayerController::TogglePersonalVehicle_Implementation slot 0x{:X}, restored in {} controller classes", Index, Swapped);
}

void AFortPlayerController::TogglePersonalVehicle_ImplementationHK(AFortPlayerController* This, bool bOn)
{
    This->TogglePersonalVehicle_Implementation(bOn);
}
