#pragma once

#include "Animation/AnimNotifies/AnimNotify.h"
#include "GameplayTagContainer.h"
#include "UObject/NameTypes.h"

// The notify an ability's montage fires where its hit lands (the Cube Monsters' FiendMeleeMontage at 0.23 s): its
// Notify (6.20 CL-4497486 0xA6CC90) has the montage's pawn trigger the ability playing it,
// AFortPawn::TriggerAbilityFromAnimation(Animation, ApplicationTag, MontageSectionIfBlocked, bSetNextSectionIfBlocked).
class UFortAnimNotify_TriggerGameplayAbility : public UAnimNotify
{
    DefineUnrealClass(UFortAnimNotify_TriggerGameplayAbility, FortniteGame)

public:
    DefineUProperty(FGameplayTag, ApplicationTag)
    DefineUProperty(FName, MontageSectionIfBlocked)
    DefineUProperty(bool, bSetNextSectionIfBlocked)
};
