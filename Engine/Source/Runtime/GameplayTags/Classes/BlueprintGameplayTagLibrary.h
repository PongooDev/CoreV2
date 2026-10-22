#pragma once

#include "GameplayTagContainer.h"
#include "UObject/Object.h"

class UBlueprintGameplayTagLibrary : public UObject
{
    DefineUnrealClass(UBlueprintGameplayTagLibrary, GameplayTags)
    DefineUFunction(AppendGameplayTagContainers)
    DefineUFunction(DoesContainerMatchTagQuery)

public:
    static void AppendGameplayTagContainers(FGameplayTagContainer& InOutTagContainer, const FGameplayTagContainer& InTagContainer);
    static bool DoesContainerMatchTagQuery(const FGameplayTagContainer& TagContainer, const FGameplayTagQuery& TagQuery);
};
