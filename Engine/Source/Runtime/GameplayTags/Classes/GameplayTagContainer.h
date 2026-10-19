#pragma once

#include "Containers/Array.h"
#include "UObject/NameTypes.h"

struct FGameplayTag
{
    FName TagName;
};

struct FGameplayTagContainer
{
    int32_t Num() const { return GameplayTags.Num(); }

    TArray<FGameplayTag> GameplayTags;
    TArray<FGameplayTag> ParentTags;
};

static_assert(sizeof(FGameplayTagContainer) == 0x20);
