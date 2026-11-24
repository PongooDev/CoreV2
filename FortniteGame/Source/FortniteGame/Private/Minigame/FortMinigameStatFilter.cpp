#include "pch.h"
#include "Minigame/FortMinigameStatFilter.h"
#include "GameplayTagContainer.h"
#include "Player/FortPlayerController.h"
#include "ReflectionParams.h"

bool UFortMinigameStatFilter::Matches(uint8_t InStatEvent, UObject* InTargetObject, AFortPlayerController* InPlayerController, const FGameplayTagContainer& InTargetTags,
    const FGameplayTagContainer& InSourceTags, const FGameplayTagContainer& InContextTags) const
{
    Reflection::FParams Params(FindStaticFunction_Matches(), true);
    if (!Params.Set(L"InStatEvent", InStatEvent) || !Params.Set(L"InTargetObject", InTargetObject) || !Params.Set(L"InPlayerController", reinterpret_cast<UObject*>(InPlayerController))
        || !Params.Set(L"InTargetTags", InTargetTags) || !Params.Set(L"InSourceTags", InSourceTags) || !Params.Set(L"InContextTags", InContextTags)
        || !Params.Call(const_cast<UFortMinigameStatFilter*>(this)))
    {
        return false;
    }
    return Params.Take<bool>(L"ReturnValue");
}
