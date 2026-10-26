#include "pch.h"
#include "Player/FortPlayerState.h"
#include "Heroes/FortHero.h"
#include "Finder.h"
#include "Hooks.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "ReflectionParams.h"

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
