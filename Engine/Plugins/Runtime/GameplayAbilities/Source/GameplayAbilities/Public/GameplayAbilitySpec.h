#pragma once

#include "Containers/Array.h"
#include "StructArray.h"
#include "Misc/CoreMiscDefines.h"
#include "Reflection.h"

#include <cstdint>

class UGameplayAbility;
class UObject;

struct FGameplayAbilitySpecHandle
{
    int32_t Handle = INDEX_NONE;

    bool IsValid() const { return Handle != INDEX_NONE; }
    bool operator==(const FGameplayAbilitySpecHandle& Other) const { return Handle == Other.Handle; }

    // The next handle from the engine's own counter (Natives::FGameplayAbilitySpecHandle_GHandleID).
    void GenerateNewHandle();
};

static_assert(sizeof(FGameplayAbilitySpecHandle) == sizeof(int32_t));

struct FGameplayAbilitySpec
{
    DefineUnrealStruct(FGameplayAbilitySpec, GameplayAbilities)
    DefineUProperty(FGameplayAbilitySpecHandle, Handle)
    DefineUProperty(UGameplayAbility*, Ability)
    DefineUProperty(int32_t, Level)
    DefineUProperty(int32_t, InputID)
    DefineUProperty(UObject*, SourceObject)
    DefineUProperty(uint8_t, ActiveCount)
    DefineBitfieldUProperty(InputPressed)
    DefineUProperty(TArray<UGameplayAbility*>, ReplicatedInstances)
    DefineUProperty(TArray<UGameplayAbility*>, NonReplicatedInstances)
};

struct FGameplayAbilitySpecContainer
{
    DefineUnrealStruct(FGameplayAbilitySpecContainer, GameplayAbilities)
    DefineUStructArrayProperty(FGameplayAbilitySpec, Items)
};
