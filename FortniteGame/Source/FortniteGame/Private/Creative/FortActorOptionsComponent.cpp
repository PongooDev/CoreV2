#include "pch.h"
#include "Creative/FortActorOptionsComponent.h"
#include "Creative/FortVolume.h"
#include "Finder.h"
#include "FortPlaylistUserOptions.h"
#include "Hooks.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortCreative, Log, All);

namespace
{
    // An exec's call of its C++ function: the `call rel32` right after P_FINISH's store of the advanced Code
    // (mov [reg+20h], r8). The call's address, or 0.
    uintptr_t ExecImplementationCall(UFunction* Function)
    {
        const uintptr_t Exec = Function ? reinterpret_cast<uintptr_t>(Function->GetNativeFunc()) : 0;
        if (!Exec || !Finder::IsReadable(reinterpret_cast<const void*>(Exec), 0x100))
        {
            return 0;
        }
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Exec);
        for (size_t Offset = 0; Offset + 9 <= 0x100; ++Offset)
        {
            if (Code[Offset] == 0x4C && Code[Offset + 1] == 0x89 && (Code[Offset + 2] & 0xC7) == 0x43 && Code[Offset + 3] == 0x20 && Code[Offset + 4] == 0xE8)
            {
                return Exec + Offset + 4;
            }
        }
        return 0;
    }
}

bool UFortActorOptionsComponent::SetPropertyOverrides(const TMap<FString, FString>& NewOptions)
{
    AActor* Owner = GetOwner();
    if (!Owner || Owner->Role != ROLE_Authority)
    {
        return false;
    }
    UPlaylistUserOptions* Options = PlayerOptions;
    if (!Options)
    {
        UE_LOG(LogFortCreative, Warning, TEXT("PLAYER OPTIONS: Missing player options data for actor target %s"), *Owner->GetName());
        return false;
    }
    TMap<FString, FString> OptionValues;
    for (const TPair<FString, FString>& Option : NewOptions)
    {
        const UPlaylistUserOptionBase* Definition = Options->GetOption(Option.Key);
        if (!Definition)
        {
            if (!Option.Value.IsEmpty())
            {
                OptionValues.Add(Option.Key, Option.Value);
            }
        }
        else if (Definition->bCanHaveNoOverride && Option.Value.IsEmpty())
        {
            OptionValues.Add(Option.Key, FString());
        }
        else if (FString Value = Definition->GetOptionValueFromName(Option.Value); !Value.IsEmpty())
        {
            OptionValues.Add(Option.Key, Value);
        }
    }
    return SetPropertyOverrideValues(OptionValues);
}

// 9.10's body is gone (the exec calls the return-zero stub); 9.00 has it inline at the end of SetPropertyOverrides
// (CL-6337466 0x12E072E..0x12E0791), which this follows.
bool UFortActorOptionsComponent::SetPropertyOverrideValues(const TMap<FString, FString>& NewOptions)
{
    AActor* Owner = GetOwner();
    if (!Owner || NewOptions.Num() <= 0 || !Natives::FPropertyOverrideData_SetPropertyOverrides(&GetMember_PlayerOptionData(), &NewOptions, true))
    {
        return false;
    }
    if (AFortVolumeManager* VolumeManager = AFortVolumeManager::Get(this))
    {
        AFortVolume* Volume = VolumeManager->GetVolumeForActor(Owner);
        if (Volume && Volume->VolumeState != EVolumeState::ReadOnly && !Volume->bNeverAllowSaving)
        {
            VolumeManager->ResaveActorWithVolume(Owner, Volume);
        }
    }
    Owner->ForceNetUpdate();
    OnAnyPropertyChanged.ProcessMulticastDelegate(nullptr);
    UE_LOG(LogFortCreative, Log, TEXT("SetPropertyOverrideValues: %s takes %d option value(s)"), *Owner->GetName(), NewOptions.Num());
    return true;
}

bool UFortActorOptionsComponent::SetPropertyOverridesCall(UFortActorOptionsComponent* This, const TMap<FString, FString>& NewOptions)
{
    return This && This->SetPropertyOverrides(NewOptions);
}

bool UFortActorOptionsComponent::SetPropertyOverrideValuesCall(UFortActorOptionsComponent* This, const TMap<FString, FString>& NewOptions)
{
    return This && This->SetPropertyOverrideValues(NewOptions);
}

// 9.10 (CL-6573057 and CL-6639283) moves the end of SetPropertyOverrides (the store into PlayerOptionData, the island
// resave, ForceNetUpdate and OnAnyPropertyChanged) into the new SetPropertyOverrideValues, and strips that: its exec
// (CL-6573057 0x1A84680) calls the return-zero stub, and SetPropertyOverrides (0x1357A50; CL-6639283 0x1357E90) builds
// the option values and returns false, so no device or prop option was ever set. Both are restored at the game's calls of
// them: ServerUpdateActorOptions_Implementation's (0x183C54E) and the two execs'. 9.01 and older have no
// SetPropertyOverrideValues and keep the game's own.
void UFortActorOptionsComponent::HookCallSites()
{
#if WITH_SERVER_CODE
    UFunction* Values = StaticClass() ? FindStaticFunction_SetPropertyOverrideValues() : nullptr;
    UFunction* Overrides = Values ? FindStaticFunction_SetPropertyOverrides() : nullptr;
    if (!Values || !Overrides)
    {
        return;
    }
    const uintptr_t ValuesCall = ExecImplementationCall(Values);
    if (!ValuesCall || !Finder::IsStubFunction(Finder::RelativeTarget(ValuesCall, 1, 5)))
    {
        Log::Info(L"Finder: UFortActorOptionsComponent::SetPropertyOverrideValues is native in this build");
        return;
    }
    const uintptr_t OverridesCall = ExecImplementationCall(Overrides);
    const uintptr_t GameSetPropertyOverrides = OverridesCall ? Finder::RelativeTarget(OverridesCall, 1, 5) : 0;

    // UPlaylistUserOptionBase::GetOptionValueFromName, which the game's SetPropertyOverrides still calls:
    // push rbx; sub rsp, 20h; cmp byte ptr [rcx+bCanHaveNoOverride], 0.
    if (GameSetPropertyOverrides && UPlaylistUserOptionBase::HasCanHaveNoOverride())
    {
        const int32_t CanHaveNoOverride = UPlaylistUserOptionBase::CanHaveNoOverrideOffset();
        for (const auto& [Site, Target] : Finder::DecodedDirectCalls(GameSetPropertyOverrides))
        {
            const uint8_t* Code = reinterpret_cast<const uint8_t*>(Target);
            if (Finder::BodyIs(Target, { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x80, 0xB9 }) && Code[12] == 0x00 && *reinterpret_cast<const int32_t*>(Code + 8) == CanHaveNoOverride)
            {
                Natives::UPlaylistUserOptionBase_GetOptionValueFromName = reinterpret_cast<decltype(Natives::UPlaylistUserOptionBase_GetOptionValueFromName)>(Target);
                break;
            }
        }
    }

    // FPropertyOverrideData::SetPropertyOverrides, through the mutator list's SetPropertyOverrides (CL-6573057 0x10D33C0),
    // which calls it with true: mov r8b, 1; lea rdx, [...]; mov rcx, rbx; call (mov [rsp+18h], r8b; mov [rsp+10h], rdx; ...).
    UFunction* MutatorListOverrides = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortMutatorListComponent:SetPropertyOverrides");
    const uintptr_t MutatorListCall = ExecImplementationCall(MutatorListOverrides);
    for (const auto& [Site, Target] : MutatorListCall ? Finder::DecodedDirectCalls(Finder::RelativeTarget(MutatorListCall, 1, 5)) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        const uint8_t* Before = reinterpret_cast<const uint8_t*>(Site - 12);
        bool bPassesTrue = false;
        for (int32_t Offset = 0; Offset + 3 <= 12 && Finder::IsReadable(Before, 12); ++Offset)
        {
            bPassesTrue |= Before[Offset] == 0x41 && Before[Offset + 1] == 0xB0 && Before[Offset + 2] == 0x01;
        }
        if (bPassesTrue && Finder::BodyIs(Target, { 0x44, 0x88, 0x44, 0x24, 0x18, 0x48, 0x89, 0x54, 0x24, 0x10 }))
        {
            Natives::FPropertyOverrideData_SetPropertyOverrides = reinterpret_cast<decltype(Natives::FPropertyOverrideData_SetPropertyOverrides)>(Target);
            break;
        }
    }

    // AFortPlayerController::ServerUpdateActorOptions_Implementation's call of the game's SetPropertyOverrides.
    UFunction* UpdateActorOptions = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerController:ServerUpdateActorOptions");
    const int32_t UpdateIndex = UpdateActorOptions ? Finder::ImplementationIndex(UpdateActorOptions) : -1;
    UObject* Default = UpdateIndex >= 0 && AFortPlayerController::StaticClass() ? AFortPlayerController::StaticClass()->GetDefaultObject(false) : nullptr;
    const uintptr_t UpdateImplementation = Default ? reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(Default))[UpdateIndex]) : 0;
    uintptr_t UpdateCall = 0;
    for (const auto& [Site, Target] : GameSetPropertyOverrides && UpdateImplementation ? Finder::DecodedDirectCalls(UpdateImplementation) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (Target == GameSetPropertyOverrides)
        {
            UpdateCall = Site;
            break;
        }
    }

    if (!GameSetPropertyOverrides || !Natives::UPlaylistUserOptionBase_GetOptionValueFromName || !Natives::FPropertyOverrideData_SetPropertyOverrides || !UpdateCall
        || !UPlaylistUserOptions::StaticClass() || !AFortVolume::HasNeverAllowSaving())
    {
        Log::Error(L"Finder: UFortActorOptionsComponent::SetPropertyOverrideValues is stripped, but its restoration is missing a part (SetPropertyOverrides {}, "
            L"GetOptionValueFromName {}, FPropertyOverrideData::SetPropertyOverrides {}, ServerUpdateActorOptions_Implementation's call {}), so device options don't save",
            GameSetPropertyOverrides != 0, Natives::UPlaylistUserOptionBase_GetOptionValueFromName != nullptr, Natives::FPropertyOverrideData_SetPropertyOverrides != nullptr, UpdateCall != 0);
        return;
    }
    Hooks::RedirectCall(L"AFortPlayerController::ServerUpdateActorOptions_Implementation -> UFortActorOptionsComponent::SetPropertyOverrides",
        reinterpret_cast<uint8_t*>(UpdateCall), reinterpret_cast<void*>(&SetPropertyOverridesCall));
    Hooks::RedirectCall(L"UFortActorOptionsComponent::execSetPropertyOverrides -> SetPropertyOverrides", reinterpret_cast<uint8_t*>(OverridesCall),
        reinterpret_cast<void*>(&SetPropertyOverridesCall));
    Hooks::RedirectCall(L"UFortActorOptionsComponent::execSetPropertyOverrideValues -> SetPropertyOverrideValues", reinterpret_cast<uint8_t*>(ValuesCall),
        reinterpret_cast<void*>(&SetPropertyOverrideValuesCall));
    const uintptr_t Base = Finder::ImageBase();
    Log::Info(L"Finder: UFortActorOptionsComponent::SetPropertyOverrideValues is stripped, restored with SetPropertyOverrides (game's +0x{:X}; "
        L"ServerUpdateActorOptions_Implementation call +0x{:X}, GetOptionValueFromName +0x{:X}, FPropertyOverrideData::SetPropertyOverrides +0x{:X})",
        GameSetPropertyOverrides - Base, UpdateCall - Base, reinterpret_cast<uintptr_t>(Natives::UPlaylistUserOptionBase_GetOptionValueFromName) - Base,
        reinterpret_cast<uintptr_t>(Natives::FPropertyOverrideData_SetPropertyOverrides) - Base);
#endif
}
