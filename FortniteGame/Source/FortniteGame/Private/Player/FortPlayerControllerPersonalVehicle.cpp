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
// turning it off activates it again, as the ability's graph is built for. 21.40 keeps the spec handle on the pawn
// (AFortPlayerPawn::PersonalVehicleAbilitySpecHandle); Core finds the spec by its ability instead, so the pawn's layout
// isn't needed.

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

    FGameplayAbilitySpecHandle Handle;
    if (const FGameplayAbilitySpec* Existing = AbilitySystem->FindAbilitySpecFromClass(AbilityClass))
    {
        Handle = Existing->Handle;
    }
    else if (bOn)
    {
        // FGameplayAbilitySpec(the ability's default object, level 1, no input, the vehicle item), with a new handle.
        UScriptStruct* SpecStruct = FGameplayAbilitySpec::StaticStruct();
        std::vector<uint8_t> SpecStorage(SpecStruct->GetStructureSize(), 0);
        SpecStruct->InitializeStruct(SpecStorage.data());
        FGameplayAbilitySpec& Spec = *reinterpret_cast<FGameplayAbilitySpec*>(SpecStorage.data());
        FGameplayAbilitySpecHandle NewHandle;
        NewHandle.GenerateNewHandle();
        Spec.Handle = NewHandle;
        Spec.Ability = static_cast<UGameplayAbility*>(AbilityClass->GetDefaultObject());
        Spec.Level = 1;
        Spec.InputID = INDEX_NONE;
        Spec.SourceObject = VehicleItem;
        Handle = AbilitySystem->GiveAbility(Spec);
        SpecStruct->DestroyStruct(SpecStorage.data());
        if (!Handle.IsValid())
        {
            Log::Warning(L"PersonalVehicle: couldn't give {} {}", GetName().ToStdString(), AbilityClass->GetName().ToStdString());
            return;
        }
        Log::Info(L"PersonalVehicle: gave {} {} ({})", GetName().ToStdString(), AbilityClass->GetName().ToStdString(), VehicleItem->GetName().ToStdString());
    }
    else
    {
        return;
    }

    // On: the ability mounts. Off: activating it again while it runs ends that activation and the graph then dismounts.
    const bool bActivated = AbilitySystem->TryActivateAbility(Handle);
    Log::Info(L"PersonalVehicle: {} {} {}{}", GetName().ToStdString(), bOn ? L"mounts" : L"dismounts", AbilityClass->GetName().ToStdString(), bActivated ? L"" : L" (activation failed)");
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
        || !Natives::UAbilitySystemComponent_GiveAbility)
    {
        // ResolveEmoteLive also finds the spec handle counter and CanPerformAction.
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
