#pragma once

#include "AbilitySystemComponent.h"

#include <cstdint>

class UAnimMontage;
class UGameplayAbility;
struct FGameplayAbilityActivationInfo;

// The shared animation an ability plays on an AI pawn that animates through the animation sharing system (6.x: the
// Fortnitemares Cube Monsters, bUseSharedAnimation): its state, the montage section and the montage
// (FGameplayAbilityRepSharedAnim, on FGameplayAbilityRepSharedAnim_Base). The pawn plays no montage of its own, so the
// montage's notifies never fire on it.
struct FGameplayAbilityRepSharedAnim
{
    DefineUnrealStruct(FGameplayAbilityRepSharedAnim, FortniteGame)
    // EFortSharedAnimationState: Anim_Attack 3, Anim_Death 4, ...
    DefineUProperty(uint8_t, AnimState)
    DefineUProperty(uint8_t, MontageSectionToPlay)
    DefineUProperty(UAnimMontage*, AnimMontage)
};

class UFortAbilitySystemComponent : public UAbilitySystemComponent
{
    DefineUnrealClass(UFortAbilitySystemComponent, FortniteGame)

public:
    // From 6.x: the shared animation the clients play for the pawn (replicated, OnRep_SharedAnim, which sets their
    // sound and feedback notifies' timers, SetCosmeticTimersFromAnimInfo, 6.20 CL-4497486 0xA74030).
    DefineUStructProperty(FGameplayAbilityRepSharedAnim, RepSharedAnimInfo)
    static bool HasRepSharedAnimInfo() { return Member_RepSharedAnimInfo.IsAvailable(); }
    static ptrdiff_t RepSharedAnimInfoOffset() { return HasRepSharedAnimInfo() ? Member_RepSharedAnimInfo.GetOffsetChecked() : -1; }

    // The shared-animation counterpart of UAbilitySystemComponent::PlayMontage, which the abilities' animation tasks call
    // as they start (UFortAbilityTask_PlayAnimWaitTarget's Activate, 6.20 0xA84EE0, and UFortAbilityTask_PlayAnimAndWait's,
    // 0xA84B40). The build keeps only RepSharedAnimInfo = SharedAnim (6.20 0xA70D50, which ignores the ability and the
    // activation it is given and returns 0); the server's part is stripped. Restored: the ability animates the montage
    // (SetCurrentMontage, as PlayMontage does), and the montage section's gameplay notifies, which the server plays no
    // animation to fire, fire by timer at their times, as the clients' cosmetic ones do. Epic's name isn't in the 21.40
    // PDB (a client's): named after PlayMontage.
    float PlaySharedAnim(UGameplayAbility* AnimatingAbility, const FGameplayAbilityActivationInfo& ActivationInfo, const FGameplayAbilityRepSharedAnim& SharedAnim);

    // From 6.x (UFortAbilityTask_PlayAnimWaitTarget, the ability task of an AI pawn that animates through the animation
    // sharing system): the tasks' calls of the build's PlaySharedAnim go to Core's (Finder::ResolveSharedAnimLive).
    // Nothing on builds without it.
    static void HookCallSites();

private:
    // The montage section's UFortAnimNotify_TriggerGameplayAbility notifies (where an attack's hit lands) fire by timer:
    // the pawn's TriggerAbilityFromAnimation, as the notify's own Notify does, at the notify's time in the section.
    // Core's name, after the clients' SetCosmeticTimersFromAnimInfo (21.40's name), which it mirrors.
    void SetGameplayTimersFromAnimInfo(UGameplayAbility* AnimatingAbility, const FGameplayAbilityRepSharedAnim& SharedAnim);

    static float PlaySharedAnimHK(UFortAbilitySystemComponent* This, UGameplayAbility* AnimatingAbility, const FGameplayAbilityActivationInfo& ActivationInfo, const FGameplayAbilityRepSharedAnim& SharedAnim);
};
