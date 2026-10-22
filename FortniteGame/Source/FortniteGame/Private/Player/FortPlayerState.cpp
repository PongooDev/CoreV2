#include "pch.h"
#include "Player/FortPlayerState.h"
#include "Heroes/FortHero.h"
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

int32_t AFortPlayerState::GetStat(EFortReplicatedStat StatType, bool bCampaign)
{
    return Natives::AFortPlayerState_GetStatIndex >= 0 ? CallVirtual<int32_t>(this, Natives::AFortPlayerState_GetStatIndex, static_cast<int32_t>(StatType), bCampaign) : 0;
}

ptrdiff_t AFortPlayerState::FinalizingExperienceOffset()
{
    return Member_bIsFinalizingExperience.GetOffsetChecked();
}

void AFortPlayerState::Hook()
{
    Hooks::Detour(L"AFortPlayerState::SetHeroType", reinterpret_cast<void*>(Natives::AFortPlayerState_SetHeroType), &SetHeroTypeHK, &SetHeroTypeOG);
}

void AFortPlayerState::SetHeroTypeHK(AFortPlayerState* This, UFortHero* Hero)
{
    This->SetHeroType(Hero);
}
