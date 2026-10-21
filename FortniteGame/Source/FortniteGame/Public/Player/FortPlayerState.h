#pragma once

#include "Customization/CustomCharacterPart.h"
#include "GameFramework/PlayerState.h"

class UFortHero;

class AFortPlayerState : public APlayerState
{
    DefineUnrealClass(AFortPlayerState, FortniteGame)

public:
    DefineUProperty(UCustomCharacterPart*, CharacterParts)
    DefineUProperty(int32_t, WorldPlayerId)
    DefineBitfieldUProperty(bIsFinalizingExperience)
    DefineBitfieldUProperty(bAreZoneStatsFinalized)
    DefineUFunction(OnRep_CharacterParts)

    UCustomCharacterPart*& GetCharacterPart(EFortCustomPartType PartType)
    {
        UCustomCharacterPart*& Parts = CharacterParts;
        return (&Parts)[static_cast<uint8_t>(PartType)];
    }

    void SetHeroType(UFortHero* Hero);
    void OnRep_CharacterParts();

    static void Hook();

private:
    static inline void (*SetHeroTypeOG)(AFortPlayerState* This, UFortHero* Hero) = nullptr;
    static void SetHeroTypeHK(AFortPlayerState* This, UFortHero* Hero);
};
