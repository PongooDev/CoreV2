#include "pch.h"
#include "Player/FortPlayerState.h"
#include "Heroes/FortHero.h"
#include "Finder.h"
#include "FortAssetManager.h"
#include "FortGameData.h"
#include "Hooks.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "ReflectionParams.h"

DEFINE_LOG_CATEGORY_STATIC(LogFort, Log, All);

void AFortPlayerState::SetHeroType(UFortHero* Hero)
{
    SetHeroTypeOG(this, Hero);

#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !Hero)
    {
        return;
    }

    const EFortCustomPartType NumTypes = EFortCustomPartType::GetNumTypes();
    if (NumTypes == Reflection::InvalidEnumValue)
    {
        return;
    }

    for (uint8_t PartType = 0; PartType < NumTypes; ++PartType)
    {
        GetCharacterPart(EFortCustomPartType{ PartType }) = nullptr;
    }

    for (UCustomCharacterPart* Part : Hero->CharacterParts)
    {
        if (Part && Part->CharacterPartType < NumTypes)
        {
            GetCharacterPart(Part->CharacterPartType) = Part;
        }
    }

    // The hero's CharacterParts only holds the parts that were already loaded when the hero was made, and a dedicated
    // server loads no body or head for anything else: a Ninja such as HID_Ninja_SlashTail_UC_T01 came out with only its
    // charms. Its pawn class (PlayerPawn_Ninja) has no mesh of its own, so the server's pawn got no skeletal mesh and
    // no anim instance, played no montage and ran no root motion: reloads ended at once on the server (#102) and Dragon
    // Slash's movement was corrected away (#103). The specializations name every part; load the missing ones.
    for (UFortHeroSpecialization* Specialization : Hero->Specializations)
    {
        const TArray<TAssetPtr<UCustomCharacterPart>>* SpecializationParts = Specialization ? &static_cast<TArray<TAssetPtr<UCustomCharacterPart>>&>(Specialization->CharacterParts) : nullptr;
        for (int32_t Index = 0; SpecializationParts && Index < SpecializationParts->Num(); ++Index)
        {
            UCustomCharacterPart* Part = (*SpecializationParts)[Index].LoadSynchronous();
            if (Part && Part->CharacterPartType < NumTypes && !GetCharacterPart(Part->CharacterPartType) && (Part->GenderPermitted & CharacterGender) != 0)
            {
                GetCharacterPart(Part->CharacterPartType) = Part;
            }
        }
    }

    // A picked-up item flew down through the player's head (#96). Clients fly it to the backpack part's loot socket
    // (the pawn's 0x7CC3B0 -> 0x504CC0: the part's UCustomCharacterBackpackData::LootSocketName), coming in along the
    // socket's direction; with no backpack part it goes to the actor's location instead, along the pawn's down vector,
    // through a point PickupSplineOffsetRange[1] (175) above it (0x681660). The native fill from the hero (1.7.2
    // 0x5014B0) gives a hero with no backpack, as every Battle Royale hero is, UFortGameData::DefaultBackpackCharacterPart
    // (NoBackpack: no mesh, the skeleton's LootEntryPosition socket on spine_04, behind the shoulders) when it permits
    // the hero's gender; Core's fill above had left the slot empty.
    const EFortCustomPartType BackpackType = EFortCustomPartType::GetBackpack();
    UFortGameData* GameData = BackpackType < NumTypes && !GetCharacterPart(BackpackType) ? UFortAssetManager::Get().GetGameData() : nullptr;
    if (UCustomCharacterPart* DefaultBackpack = GameData ? GameData->DefaultBackpackCharacterPart.LoadSynchronous() : nullptr)
    {
        if (DefaultBackpack->CharacterPartType == BackpackType && (DefaultBackpack->GenderPermitted & CharacterGender) != 0)
        {
            GetCharacterPart(BackpackType) = DefaultBackpack;
            UE_LOG(LogFort, Log, TEXT("SetHeroType: %s's hero has no backpack, it takes the default %s"), *GetName(), *DefaultBackpack->GetName());
        }
        else
        {
            UE_LOG(LogFort, Warning, TEXT("SetHeroType: %s doesn't take the default backpack %s"), *GetName(), *DefaultBackpack->GetName());
        }
    }

    OnRep_CharacterParts();
#endif
}

void AFortPlayerState::OnRep_CharacterParts()
{
    Reflection::FParams Params(StaticFunction_OnRep_CharacterParts());
    Params.Call(this);
}

void AFortPlayerStateZone::ModifyStat(EFortReplicatedStat StatType, int32_t StatDelta, int32_t ScoreDelta, const FVector& Location)
{
    if (StatType == Reflection::InvalidEnumValue || (Natives::AFortPlayerStateZone_ModifyStatIndex < 0 && !Finder::ResolveBuildingLootLive()))
    {
        return;
    }
    using FModifyStat = void (*)(AFortPlayerStateZone* This, uint8_t StatType, int32_t StatDelta, int32_t ScoreDelta, const FVector* Location);
    reinterpret_cast<FModifyStat>((*reinterpret_cast<void***>(this))[Natives::AFortPlayerStateZone_ModifyStatIndex])(this, StatType, StatDelta, ScoreDelta, &Location);
}

void AFortPlayerStateZone::AddAccumulatedItem(UFortItemDefinition* ItemDefinition, int32_t Delta)
{
#if WITH_SERVER_CODE
    if (Role != ROLE_Authority || !ItemDefinition || Delta == 0)
    {
        return;
    }

    TStructArray<FAccumulatedItemEntry>& Items = AccumulatedItems;
    int32_t Index = 0;
    while (Index < Items.Num() && static_cast<UFortItemDefinition*>(Items[Index].ItemDefinition) != ItemDefinition)
    {
        ++Index;
    }
    if (Index == Items.Num())
    {
        if (Delta < 0)
        {
            return;
        }
        FAccumulatedItemEntry& NewEntry = Items.AddDefaulted();
        NewEntry.ItemDefinition = ItemDefinition;
    }

    FAccumulatedItemEntry& Entry = Items[Index];
    Entry.Quantity = Entry.Quantity + Delta;
    if (Entry.Quantity <= 0)
    {
        Items.RemoveAt(Index);
    }

    ForceNetUpdate();
    OnRep_AccumulatedItems();
#endif
}

void AFortPlayerStateZone::OnRep_AccumulatedItems()
{
    Reflection::FParams Params(StaticFunction_OnRep_AccumulatedItems());
    Params.Call(this);
}

int32_t AFortPlayerState::GetStat(EFortReplicatedStat StatType, bool bCampaign)
{
    return Natives::AFortPlayerState_GetStatIndex >= 0 ? CallVirtual<int32_t>(this, Natives::AFortPlayerState_GetStatIndex, static_cast<int32_t>(StatType), bCampaign) : 0;
}

bool AFortPlayerState::HasFinalizingExperience()
{
    return Member_bIsFinalizingExperience.IsAvailable();
}

void AFortPlayerState::Hook()
{
    Hooks::Detour(L"AFortPlayerState::SetHeroType", reinterpret_cast<void*>(Natives::AFortPlayerState_SetHeroType), &SetHeroTypeHK, &SetHeroTypeOG);
}

void AFortPlayerState::SetHeroTypeHK(AFortPlayerState* This, UFortHero* Hero)
{
    This->SetHeroType(Hero);
}
