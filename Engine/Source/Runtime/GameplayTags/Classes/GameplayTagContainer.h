#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "UObject/NameTypes.h"

#include <string>

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

    // Adds the tag and its parents (its dotted prefixes, as UGameplayTagsManager registers them).
    void AddTag(const FGameplayTag& TagToAdd)
    {
        if (!TagToAdd.IsValid() || GameplayTags.Contains(TagToAdd))
        {
            return;
        }
        GameplayTags.Add(TagToAdd);
        const std::wstring Name = TagToAdd.TagName.ToStdString();
        for (size_t Dot = Name.rfind(L'.'); Dot != std::wstring::npos && Dot > 0; Dot = Name.rfind(L'.', Dot - 1))
        {
            const FGameplayTag Parent{ FName(Name.substr(0, Dot).c_str()) };
            if (!ParentTags.Contains(Parent))
            {
                ParentTags.Add(Parent);
            }
        }
    }

    // Matches the tag itself or one of its parents, as the native loot filters compare the container's two arrays.
    bool HasTag(const FGameplayTag& Tag) const
    {
        return GameplayTags.Contains(Tag) || ParentTags.Contains(Tag);
    }

    // Every tag of the other container matches here (an empty one always does), as UE's HasAll.
    bool HasAll(const FGameplayTagContainer& ContainerToCheck) const
    {
        for (const FGameplayTag& OtherTag : ContainerToCheck.GameplayTags)
        {
            if (!HasTag(OtherTag))
            {
                return false;
            }
        }
        return true;
    }

    TArray<FGameplayTag> GameplayTags;
    TArray<FGameplayTag> ParentTags;
};

static_assert(sizeof(FGameplayTagContainer) == 0x20);

struct FGameplayTagQuery
{
    bool IsEmpty() const { return QueryTokenStream.Num() == 0; }

    // ALL( InTags ): the stream version, a root expression, EGameplayTagQueryExprType::AllTagsMatch (2), the tag count and
    // each tag's index in the dictionary.
    static FGameplayTagQuery MakeQuery_MatchAllTags(const FGameplayTagContainer& InTags)
    {
        FGameplayTagQuery Query{};
        Query.TokenStreamVersion = 0;
        for (const uint8_t Token : { uint8_t(0), uint8_t(1), uint8_t(2), static_cast<uint8_t>(InTags.GameplayTags.Num()) })
        {
            Query.QueryTokenStream.Add(Token);
        }
        for (int32_t Index = 0; Index < InTags.GameplayTags.Num(); ++Index)
        {
            Query.TagDictionary.Add(InTags.GameplayTags[Index]);
            Query.QueryTokenStream.Add(static_cast<uint8_t>(Index));
        }
        return Query;
    }

    int32_t TokenStreamVersion;
    TArray<FGameplayTag> TagDictionary;
    TArray<uint8_t> QueryTokenStream;
    FString UserDescription;
    FString AutoDescription;
};

static_assert(sizeof(FGameplayTagQuery) == 0x48);
