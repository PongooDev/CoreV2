#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
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

struct FGameplayTagQuery
{
    bool IsEmpty() const { return QueryTokenStream.Num() == 0; }

    int32_t TokenStreamVersion;
    TArray<FGameplayTag> TagDictionary;
    TArray<uint8_t> QueryTokenStream;
    FString UserDescription;
    FString AutoDescription;
};

static_assert(sizeof(FGameplayTagQuery) == 0x48);
