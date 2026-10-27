#include "pch.h"
#include "Heroes/FortHeroType.h"
#include "GameplayEffect.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

float UFortHeroType::GetCombinedStatBonus(const UProperty* Attribute) const
{
    float Bonus = 0.0f;
    const TStructArray<FGameplayEffectApplicationInfo>& Effects = CombinedStatGEs;
    for (int32_t Index = 0; Attribute && Index < Effects.Num(); ++Index)
    {
        // Heroes keep empty entries (every HID_Commando_Athena_* starts with one).
        const TAssetSubclassOf<UGameplayEffect>& EffectClass = Effects[Index].GameplayEffect;
        UClass* Class = EffectClass.LoadSynchronous();
        UGameplayEffect* Effect = Class ? Cast<UGameplayEffect>(Class->GetDefaultObject()) : nullptr;
        if (!Effect)
        {
            continue;
        }

        const TStructArray<FGameplayModifierInfo>& Modifiers = Effect->Modifiers;
        for (int32_t ModifierIndex = 0; ModifierIndex < Modifiers.Num(); ++ModifierIndex)
        {
            Bonus += Modifiers[ModifierIndex].GetAdditiveConstant(Attribute);
        }
    }
    return Bonus;
}
