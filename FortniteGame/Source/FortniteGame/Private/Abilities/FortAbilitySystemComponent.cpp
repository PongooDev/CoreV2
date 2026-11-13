#include "pch.h"
#include "Abilities/FortAbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Animation/AnimMontage.h"
#include "Animation/FortAnimNotify_TriggerGameplayAbility.h"
#include "Engine/EngineTypes.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Actor.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Pawns/FortPawn.h"
#include "Templates/Casts.h"
#include "TimerManager.h"
#include "UObject/WeakObjectPtr.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace
{
    // The gameplay notify timers each ability system has pending: a shared animation the system plays next takes the
    // place of the last one, whose notifies yet to fire then never do (as a montage played over another's).
    struct FPendingNotifyTimers
    {
        TWeakObjectPtr<UFortAbilitySystemComponent> AbilitySystem;
        std::vector<FTimerHandle> Handles;
    };

    std::unordered_map<const UFortAbilitySystemComponent*, FPendingNotifyTimers> PendingNotifyTimers;

    std::vector<FTimerHandle>& PendingTimersOf(UFortAbilitySystemComponent* AbilitySystem, FTimerManager& TimerManager)
    {
        // An ability system that is gone leaves its entry; such entries go once there are many.
        if (PendingNotifyTimers.size() > 256)
        {
            std::erase_if(PendingNotifyTimers, [](const auto& Entry) { return !Entry.second.AbilitySystem.Get(); });
        }
        FPendingNotifyTimers& Pending = PendingNotifyTimers[AbilitySystem];
        if (Pending.AbilitySystem.Get() == AbilitySystem)
        {
            for (FTimerHandle& Handle : Pending.Handles)
            {
                TimerManager.ClearTimer(Handle);
            }
        }
        Pending.AbilitySystem = AbilitySystem;
        Pending.Handles.clear();
        return Pending.Handles;
    }
}

float UFortAbilitySystemComponent::PlaySharedAnim(UGameplayAbility* AnimatingAbility, const FGameplayAbilityActivationInfo& ActivationInfo, const FGameplayAbilityRepSharedAnim& SharedAnim)
{
    // What the build keeps: the shared animation for the clients (RepSharedAnimInfo, OnRep_SharedAnim).
    const float Duration = Natives::UFortAbilitySystemComponent_PlaySharedAnim(this, AnimatingAbility, &ActivationInfo, &SharedAnim);
#if WITH_SERVER_CODE
    SetGameplayTimersFromAnimInfo(AnimatingAbility, SharedAnim);
#endif
    return Duration;
}

void UFortAbilitySystemComponent::SetGameplayTimersFromAnimInfo(UGameplayAbility* AnimatingAbility, const FGameplayAbilityRepSharedAnim& SharedAnim)
{
#if WITH_SERVER_CODE
    UAnimMontage* const Montage = SharedAnim.AnimMontage;
    AActor* const Avatar = HasAvatarActor() ? static_cast<AActor*>(AvatarActor) : static_cast<AActor*>(OwnerActor);
    AFortPawn* const Pawn = Cast<AFortPawn>(reinterpret_cast<UObject*>(Avatar));
    UWorld* const World = GetWorld();
    if (!Montage || !AnimatingAbility || !Pawn || Pawn->Role != ROLE_Authority || !World)
    {
        return;
    }

    // The ability animates the montage, as UAbilitySystemComponent::PlayMontage has it (SetCurrentMontage): the pawn's
    // TriggerAbilityFromAnimation finds the ability a notify is for by its current montage.
    AnimatingAbility->CurrentMontage = Montage;

    FTimerManager& TimerManager = World->GetTimerManager();
    std::vector<FTimerHandle>& Handles = PendingTimersOf(this, TimerManager);

    // As SetCosmeticTimersFromAnimInfo (6.20 CL-4497486 0xA74030) does for the clients' sound and feedback notifies: each
    // notify in the montage section, after as long as it is into the section. Here the gameplay ones, where an attack's
    // hit lands (UFortAnimNotify_TriggerGameplayAbility, whose Notify, 0xA6CC90, has the pawn trigger the ability).
    float SectionStart = 0.0f;
    float SectionEnd = 0.0f;
    Montage->GetSectionStartAndEndTime(SharedAnim.MontageSectionToPlay, SectionStart, SectionEnd);
    TStructArray<FAnimNotifyEvent>& Notifies = Montage->GetMember_Notifies();
    int32_t Triggers = 0;
    float FirstDelay = -1.0f;
    for (int32_t Index = 0; Index < Notifies.Num(); ++Index)
    {
        const FAnimNotifyEvent& Event = Notifies[Index];
        UFortAnimNotify_TriggerGameplayAbility* const Notify = Cast<UFortAnimNotify_TriggerGameplayAbility>(reinterpret_cast<UObject*>(static_cast<UAnimNotify*>(Event.Notify)));
        const float TriggerTime = Notify ? Event.GetTriggerTime() : -1.0f;
        if (!Notify || TriggerTime < SectionStart || TriggerTime > SectionEnd)
        {
            continue;
        }

        const TWeakObjectPtr<UFortAbilitySystemComponent> WeakAbilitySystem(this);
        const TWeakObjectPtr<UGameplayAbility> WeakAbility(AnimatingAbility);
        const TWeakObjectPtr<AFortPawn> WeakPawn(Pawn);
        const TWeakObjectPtr<UAnimMontage> WeakMontage(Montage);
        const FGameplayTag ApplicationTag = Notify->ApplicationTag;
        const FName MontageSectionIfBlocked = Notify->MontageSectionIfBlocked;
        const bool bSetNextSectionIfBlocked = Notify->bSetNextSectionIfBlocked;
        const FTimerDelegate Fire = FTimerDelegate::CreateLambda([WeakAbilitySystem, WeakAbility, WeakPawn, WeakMontage, ApplicationTag, MontageSectionIfBlocked, bSetNextSectionIfBlocked]()
        {
            // Only while the ability still plays this animation: it ended (a stun, the pawn's death), or the system went
            // on to another shared animation (SetSharedAnimState's death or hit reaction, which takes the montage away).
            UFortAbilitySystemComponent* const AbilitySystem = WeakAbilitySystem.Get();
            UGameplayAbility* const Ability = WeakAbility.Get();
            AFortPawn* const Owner = WeakPawn.Get();
            UAnimMontage* const PlayedMontage = WeakMontage.Get();
            if (!AbilitySystem || !Ability || !Owner || !PlayedMontage || !Ability->bIsActive || Ability->CurrentMontage != PlayedMontage
                || AbilitySystem->RepSharedAnimInfo.AnimMontage != PlayedMontage || Owner->IsDead())
            {
                return;
            }
            Owner->TriggerAbilityFromAnimation(reinterpret_cast<UAnimSequenceBase*>(PlayedMontage), ApplicationTag, MontageSectionIfBlocked, bSetNextSectionIfBlocked);
        });

        const float Delay = TriggerTime - SectionStart;
        FTimerHandle Handle;
        if (Delay > 0.0f)
        {
            TimerManager.SetTimer(Handle, Fire, Delay, false);
        }
        else
        {
            Handle = TimerManager.SetTimerForNextTick(Fire);
        }
        Handles.push_back(Handle);
        FirstDelay = Triggers++ == 0 ? Delay : std::min(FirstDelay, Delay);
    }

    static bool bLogged = false;
    if (!bLogged && Triggers > 0)
    {
        bLogged = true;
        Log::Info(L"Abilities: {}'s {} plays {} (section {}, {:.2f} s to {:.2f} s) as a shared animation; its {} gameplay notifies fire by timer on the server, the first after {:.2f} s",
            Pawn->GetName().ToStdString(), AnimatingAbility->GetName().ToStdString(), Montage->GetName().ToStdString(), static_cast<int32_t>(SharedAnim.MontageSectionToPlay),
            SectionStart, SectionEnd, Triggers, FirstDelay);
    }
#endif
}

float UFortAbilitySystemComponent::PlaySharedAnimHK(UFortAbilitySystemComponent* This, UGameplayAbility* AnimatingAbility, const FGameplayAbilityActivationInfo& ActivationInfo, const FGameplayAbilityRepSharedAnim& SharedAnim)
{
    return This->PlaySharedAnim(AnimatingAbility, ActivationInfo, SharedAnim);
}

void UFortAbilitySystemComponent::HookCallSites()
{
#if WITH_SERVER_CODE
    if (!HasRepSharedAnimInfo() || !Finder::ResolveSharedAnimLive(RepSharedAnimInfoOffset()))
    {
        return;
    }
    for (uint8_t* Call : Natives::UFortAbilitySystemComponent_PlaySharedAnimCalls)
    {
        Hooks::RedirectCall(L"UFortAbilityTask PlayAnim -> UFortAbilitySystemComponent::PlaySharedAnim", Call, reinterpret_cast<void*>(&PlaySharedAnimHK));
    }
#endif
}
