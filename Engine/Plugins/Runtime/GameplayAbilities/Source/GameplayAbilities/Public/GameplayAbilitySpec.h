#pragma once

#include "Containers/ScriptArray.h"
#include "Misc/CoreMiscDefines.h"
#include "Reflection.h"

#include <cstdint>

class UGameplayAbility;

struct FGameplayAbilitySpecHandle
{
    int32_t Handle = INDEX_NONE;

    bool IsValid() const { return Handle != INDEX_NONE; }
    bool operator==(const FGameplayAbilitySpecHandle& Other) const { return Handle == Other.Handle; }
};

static_assert(sizeof(FGameplayAbilitySpecHandle) == sizeof(int32_t));

struct FGameplayAbilitySpec
{
    DefineUnrealStruct(FGameplayAbilitySpec, GameplayAbilities)
    DefineUProperty(FGameplayAbilitySpecHandle, Handle)
    DefineUProperty(UGameplayAbility*, Ability)
    DefineBitfieldUProperty(InputPressed)
};

struct FGameplayAbilitySpecContainer
{
    DefineUnrealStruct(FGameplayAbilitySpecContainer, GameplayAbilities)
    DefineUProperty(FScriptArray, Items)
};
