#include "pch.h"
#include "FortGlobals.h"
#include "Abilities/FortAbilitySet.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "FortKismetLibrary.h"
#include "GameFramework/Actor.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

void UFortGlobals::ApplyHeroEffectsAndAbilities(UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles)
{
    ApplyHeroEffectsAndAbilitiesOG(Hero, AbilitySystem, OutEffectHandles, OutAbilitySetHandles);
    EquipGenericPlayerAbilitySet(AbilitySystem, OutAbilitySetHandles);
}

void UFortGlobals::ApplyHeroEffectsAndAbilities(UFortCampaignHeroLoadoutItem* HeroLoadout, UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles,
    FScriptArray& OutAbilitySetHandles)
{
    ApplyHeroEffectsAndAbilitiesWithLoadoutOG(HeroLoadout, Hero, AbilitySystem, OutEffectHandles, OutAbilitySetHandles);
    EquipGenericPlayerAbilitySet(AbilitySystem, OutAbilitySetHandles);
}

void UFortGlobals::EquipGenericPlayerAbilitySet(IAbilitySystemInterface* AbilitySystem, FScriptArray& OutAbilitySetHandles)
{
#if WITH_SERVER_CODE
    UAbilitySystemComponent* AbilitySystemComponent = AbilitySystem ? AbilitySystem->GetAbilitySystemComponent() : nullptr;
    AActor* OwnerActor = AbilitySystemComponent ? AbilitySystemComponent->OwnerActor : nullptr;
    if (!OwnerActor || OwnerActor->Role != ROLE_Authority)
    {
        return;
    }

    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UFortAbilitySet* GenericPlayerAbilitySet = GameData ? GameData->GenericPlayerAbilitySet.LoadSynchronous() : nullptr;
    if (!GenericPlayerAbilitySet)
    {
        return;
    }

    const int32_t HandleSize = FFortAbilitySetHandle::StaticStruct()->GetStructureSize();
    const int32_t Index = OutAbilitySetHandles.AddZeroed(1, HandleSize);
    FFortAbilitySetHandle& Handle = *reinterpret_cast<FFortAbilitySetHandle*>(static_cast<uint8_t*>(OutAbilitySetHandles.GetData()) + static_cast<size_t>(Index) * HandleSize);
    UFortKismetLibrary::EquipFortAbilitySet(TScriptInterface<IAbilitySystemInterface>(AbilitySystem->_getUObject(), AbilitySystem), GenericPlayerAbilitySet, nullptr, Handle);
#endif
}

void UFortGlobals::InitializePlayerGameplayAbilities(IAbilitySystemInterface* PlayerStateOrProxy)
{
#if WITH_SERVER_CODE
    // 8.00 (CL-5203069): AFortGameState::ApplyHomebaseEffectsOnPlayerSetup (0x1053430) calls ApplyHeroEffectsAndAbilities
    // only when its subgame test (0x109C0B0, ESubGame::Campaign) passes, so in Battle Royale the restored grant there
    // (EquipGenericPlayerAbilitySet) never ran and players had no GAS_DefaultPlayer: no jump, sprint or the rest.
    // 7.30 (0xFF45F0) made the call in every mode. Save the World still gets the set from ApplyHeroEffectsAndAbilities.
    if (!Natives::UFortGlobals_ApplyHeroEffectsAndAbilitiesCampaignOnly || !PlayerStateOrProxy)
    {
        return;
    }
    UAbilitySystemComponent* AbilitySystemComponent = PlayerStateOrProxy->GetAbilitySystemComponent();
    AActor* OwnerActor = AbilitySystemComponent ? AbilitySystemComponent->OwnerActor : nullptr;
    if (!OwnerActor || OwnerActor->Role != ROLE_Authority)
    {
        return;
    }

    // 8.40 (CL-6005771, 0x1293330) gives the Athena sub game's players the game data's AthenaPlayerAbilitySet
    // (GAS_AthenaPlayer) and the others GenericPlayerAbilitySet, by UFortGlobals::GetSubGame (0x128A980) == Athena; 8.30
    // (0x1276010) gives GenericPlayerAbilitySet only. GA_Rift_Athena_Skydive is only in GAS_AthenaPlayer on 8.40, so with
    // GAS_DefaultPlayer a rift's Abilities.Generic.Rift.Trigger found no ability and the player fell standing (#613).
    // Core calls this only in Battle Royale and Creative (AFortGameModeAthena), the Athena sub game.
    UFortGameData* GameData = UFortAssetManager::Get().GetGameData();
    UFortAbilitySet* PlayerAbilitySet = nullptr;
    if (GameData && UFortGameData::HasAthenaPlayerAbilitySet())
    {
        PlayerAbilitySet = GameData->AthenaPlayerAbilitySet.LoadSynchronous();
    }
    if (!PlayerAbilitySet && GameData)
    {
        PlayerAbilitySet = GameData->GenericPlayerAbilitySet.LoadSynchronous();
    }
    if (!PlayerAbilitySet)
    {
        return;
    }

    // The set stays on the player state's ability system across pawns (the bus, a respawn): equip it again only when
    // one of its abilities is missing, so its effects aren't applied twice.
    bool bHasAll = true;
    const TArray<UClass*>& Abilities = PlayerAbilitySet->GameplayAbilities;
    for (UClass* Ability : Abilities)
    {
        if (Ability && !AbilitySystemComponent->FindAbilitySpecFromClass(Ability))
        {
            bHasAll = false;
            break;
        }
    }
    if (bHasAll)
    {
        return;
    }

    UScriptStruct* HandleStruct = FFortAbilitySetHandle::StaticStruct();
    TArray<uint8_t> HandleStorage;
    HandleStorage.AddZeroed(HandleStruct->GetStructureSize());
    HandleStruct->InitializeStruct(HandleStorage.GetData());
    UFortKismetLibrary::EquipFortAbilitySet(TScriptInterface<IAbilitySystemInterface>(PlayerStateOrProxy->_getUObject(), PlayerStateOrProxy), PlayerAbilitySet, nullptr,
        *reinterpret_cast<FFortAbilitySetHandle*>(HandleStorage.GetData()));
    HandleStruct->DestroyStruct(HandleStorage.GetData());
    UE_LOG(LogFort, Log, TEXT("InitializePlayerGameplayAbilities: %s has %s (%d abilities)"), *GetNameSafe(OwnerActor), *GetNameSafe(PlayerAbilitySet), Abilities.Num());
#endif
}

void UFortGlobals::Hook()
{
    // 7.00 (CL-4629139) takes the hero loadout first: the detour has to read the game's arguments where they are (#472).
    if (Natives::UFortGlobals_ApplyHeroEffectsAndAbilitiesTakesHeroLoadout)
    {
        void (*const Detour)(UFortCampaignHeroLoadoutItem*, UFortHero*, IAbilitySystemInterface*, TArray<FActiveGameplayEffectHandle>&, FScriptArray&) = &ApplyHeroEffectsAndAbilities;
        Hooks::Detour(L"UFortGlobals::ApplyHeroEffectsAndAbilities", reinterpret_cast<void*>(Natives::UFortGlobals_ApplyHeroEffectsAndAbilities), Detour, &ApplyHeroEffectsAndAbilitiesWithLoadoutOG);
        return;
    }
    void (*const Detour)(UFortHero*, IAbilitySystemInterface*, TArray<FActiveGameplayEffectHandle>&, FScriptArray&) = &ApplyHeroEffectsAndAbilities;
    Hooks::Detour(L"UFortGlobals::ApplyHeroEffectsAndAbilities", reinterpret_cast<void*>(Natives::UFortGlobals_ApplyHeroEffectsAndAbilities), Detour, &ApplyHeroEffectsAndAbilitiesOG);
}
