#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

class UBlueprintGameplayTagLibrary : public UObject
{
    DefineUnrealClass(UBlueprintGameplayTagLibrary, GameplayTags)
    DefineUFunction(AppendGameplayTagContainers)

public:
    static void AppendGameplayTagContainers(FGameplayTagContainer& InOutTagContainer, const FGameplayTagContainer& InTagContainer);
};
