#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "UObject/NameTypes.h"

struct FGameplayTag
{
    bool IsValid() const { return !TagName.IsNone(); }
    bool operator==(const FGameplayTag& Other) const { return TagName == Other.TagName; }

    FName TagName;
};

struct FGameplayTagContainer
{
    int32_t Num() const { return GameplayTags.Num(); }

    void AppendTags(const FGameplayTagContainer& Other)
    {
        for (const FGameplayTag& OtherTag : Other.GameplayTags)
        {
            if (!GameplayTags.Contains(OtherTag))
            {
                GameplayTags.Add(OtherTag);
            }
        }
        for (const FGameplayTag& OtherTag : Other.ParentTags)
        {
            if (!ParentTags.Contains(OtherTag))
            {
                ParentTags.Add(OtherTag);
            }
        }
    }

    // Matches the tag itself or one of its parents, as the native loot filters compare the container's two arrays.
    bool HasTag(const FGameplayTag& Tag) const
    {
        return GameplayTags.Contains(Tag) || ParentTags.Contains(Tag);
    }

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
