#include "pch.h"
#include "AbilitySystemComponent.h"
#include "Delegates/Delegate.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"
#include "UObject/Class.h"

#include <optional>
#include <vector>

FOnGameplayAttributeChange* UAbilitySystemComponent::RegisterGameplayAttributeEvent(FGameplayAttribute Attribute)
{
    return HasGameplayAttributeEvents() ? static_cast<FActiveGameplayEffectsContainer&>(ActiveGameplayEffects).RegisterGameplayAttributeEvent(std::move(Attribute)) : nullptr;
}

bool UAbilitySystemComponent::ResolveNatives()
{
    static std::optional<bool> bResolved;
    if (bResolved)
    {
        return *bResolved;
    }
    bResolved = false;

    const auto Rva = [](uintptr_t Address) { return Address ? Address - Finder::ImageBase() : 0; };
    const ptrdiff_t ContainerOffset = Member_ActiveGameplayEffects.IsAvailable() ? Member_ActiveGameplayEffects.GetOffsetChecked() : -1;
    UStruct* const Super = StaticClass() ? StaticClass()->GetSuperStruct() : nullptr;
    const ptrdiff_t SuperSize = Super ? Super->GetPropertiesSize() : 0;

    // FActiveGameplayEffectsContainer::AddActiveGameplayEffectGrantedTagsAndModifiers (5.30 0x648670, 5.40 0x6519C0, 4.4
    // 0x5B9170, 3.2 0x51FB40, 1.8.1 0x3D84E50) ends with Owner->OnActiveGameplayEffectAddedDelegateToSelf.Broadcast(Owner,
    // Effect.Spec, Effect.Handle): inlined from UE 4.21, a call before. Epic declares it after UGameplayTasksComponent's
    // members (OnGameplayEffectAppliedDelegateToSelf and ...ToTarget before it) and before ActiveGameplayEffects.
    const std::vector<uintptr_t> AddedRefs = Finder::StringRefs(L"AddActiveGameplayEffectGrantedTagsAndModifiers called with null Def!");
    const uintptr_t AddTagsAndModifiers = AddedRefs.empty() ? 0 : Finder::FunctionStart(AddedRefs.front());
    const ptrdiff_t AddedDelegate = AddTagsAndModifiers ? Finder::LastBroadcastDelegateOffset(AddTagsAndModifiers) : -1;
    // The ability system's default object has it as Epic's constructor left it.
    UObject* const AbilitySystemDefault = StaticClass() ? StaticClass()->GetDefaultObject(false) : nullptr;
    if (AddedDelegate > SuperSize && ContainerOffset > AddedDelegate && AbilitySystemDefault
        && FMulticastDelegateBase::IsDefaultConstructedAt(reinterpret_cast<const uint8_t*>(AbilitySystemDefault) + AddedDelegate))
    {
        Natives::UAbilitySystemComponent_OnActiveGameplayEffectAddedDelegateToSelf = AddedDelegate;
    }
    else
    {
        Log::Error(L"Finder: UAbilitySystemComponent::OnActiveGameplayEffectAddedDelegateToSelf not found (AddActiveGameplayEffectGrantedTagsAndModifiers 0x{:X}, broadcast +0x{:X}, between +0x{:X} and ActiveGameplayEffects +0x{:X})",
            Rva(AddTagsAndModifiers), AddedDelegate, SuperSize, ContainerOffset);
    }

    // FActiveGameplayEffectsContainer::InternalUpdateNumericalAttribute (5.30 0x65EC10, 4.4 0x5D07F0, 3.2 0x536AB0, 1.8.1
    // 0x3D9A0D0) logs the new value, then looks the attribute up in AttributeChangeDelegates (lea rcx, [rdi+148h]; call
    // FindId) to broadcast it (and from 4.17 in AttributeValueChangeDelegates the same way, after it). RegisterGameplay
    // AttributeEvent is the small FindOrAdd on that map (5.30 0x667D20, 4.4 0x5D8B50, 3.2 0x53E000, 1.8.1 0x3DA1080:
    // lea rbx, [rcx+Map]; FindId; Emplace when missing), the only other caller of that FindId to take the map off its
    // first argument.
    const std::vector<uintptr_t> UpdateRefs = Finder::StringRefs(L"Property %s new value is: %.2f");
    const uintptr_t InternalUpdate = UpdateRefs.empty() ? 0 : Finder::FunctionStart(UpdateRefs.front());
    const std::vector<std::pair<ptrdiff_t, uintptr_t>> Lookups = InternalUpdate ? Finder::MemberThisCalls(InternalUpdate) : std::vector<std::pair<ptrdiff_t, uintptr_t>>();
    const ptrdiff_t MapOffset = Lookups.empty() ? -1 : Lookups.front().first;
    const uintptr_t FindId = Lookups.empty() ? 0 : Lookups.front().second;
    uintptr_t FindOrAdd = 0;
    int32_t Candidates = 0;
    for (const uintptr_t Call : FindId && MapOffset > 0 ? Finder::CallsTo(FindId, false) : std::vector<uintptr_t>())
    {
        const uintptr_t Caller = Finder::FunctionStart(Call);
        if (!Caller || Caller == InternalUpdate || Caller == FindOrAdd || Call - Caller > 0x40)
        {
            continue;
        }
        // lea r64, [rcx+Map] (REX.W 8D, ModRM mod 10 with rcx as base) or add rcx, Map (48 81 C1) before the call.
        bool bTakesMap = false;
        for (uintptr_t Site = Caller; Site + 7 <= Call && !bTakesMap; ++Site)
        {
            const uint8_t* Code = reinterpret_cast<const uint8_t*>(Site);
            const bool bLea = (Code[0] & 0xFB) == 0x48 && Code[1] == 0x8D && (Code[2] & 0xC7) == 0x81;
            const bool bAdd = Code[0] == 0x48 && Code[1] == 0x81 && Code[2] == 0xC1;
            bTakesMap = (bLea || bAdd) && *reinterpret_cast<const int32_t*>(Site + 3) == MapOffset;
        }
        if (bTakesMap)
        {
            FindOrAdd = Caller;
            ++Candidates;
        }
    }
    if (Candidates == 1)
    {
        Natives::FActiveGameplayEffectsContainer_RegisterGameplayAttributeEvent = reinterpret_cast<decltype(Natives::FActiveGameplayEffectsContainer_RegisterGameplayAttributeEvent)>(FindOrAdd);
    }
    else
    {
        Log::Error(L"Finder: FActiveGameplayEffectsContainer::RegisterGameplayAttributeEvent not found (InternalUpdateNumericalAttribute 0x{:X}, AttributeChangeDelegates +0x{:X}, FindId 0x{:X}, {} candidates)",
            Rva(InternalUpdate), MapOffset, Rva(FindId), Candidates);
    }

    Log::Info(L"Finder: UAbilitySystemComponent::OnActiveGameplayEffectAddedDelegateToSelf +0x{:X}, FActiveGameplayEffectsContainer::RegisterGameplayAttributeEvent 0x{:X} (AttributeChangeDelegates +0x{:X}, ActiveGameplayEffects +0x{:X})",
        Natives::UAbilitySystemComponent_OnActiveGameplayEffectAddedDelegateToSelf, Rva(FindOrAdd), MapOffset, ContainerOffset);
    bResolved = Natives::UAbilitySystemComponent_OnActiveGameplayEffectAddedDelegateToSelf > 0 && Natives::FActiveGameplayEffectsContainer_RegisterGameplayAttributeEvent;
    return *bResolved;
}
