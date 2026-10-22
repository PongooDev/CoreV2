#pragma once

#include "Customization/CustomCharacterPart.h"
#include "GameFramework/PlayerState.h"

class AFortTeamInfo;
class UAbilitySystemComponent;
class UFortHero;

namespace EFortReplicatedStat
{
    enum Type
    {
        MonsterKills,
        MonsterDamagePoints,
        PlayerKills,
        WoodGathered,
        StoneGathered,
        MetalGathered,
        Deaths,
        BuildingsBuilt,
        BuildingsBuilt_Wood,
        BuildingsBuilt_Stone,
        BuildingsBuilt_Metal,
        BuildingsUpgraded_Wood2,
        BuildingsUpgraded_Wood3,
        BuildingsUpgraded_Stone2,
        BuildingsUpgraded_Stone3,
        BuildingsUpgraded_Metal2,
        BuildingsUpgraded_Metal3,
        BuildingsDestroyed,
        Repair_Wood,
        Repair_Stone,
        Repair_Metal,
        FlagsCaptured,
        FlagsReturned,
        ContainersLooted,
        CraftingPoints,
        TrapPlacementPoints,
        TrapActivationPoints,
        TotalScore,
        OldTotalScore,
        CombatScore,
        BuildingScore,
        UtilityScore,
        BadgesScore,
        None,
        MAX,
    };
}

class AFortPlayerState : public APlayerState
{
    DefineUnrealClass(AFortPlayerState, FortniteGame)

public:
    DefineUProperty(UAbilitySystemComponent*, AbilitySystemComponent)
    DefineUProperty(UCustomCharacterPart*, CharacterParts)
    DefineUProperty(int32_t, WorldPlayerId)
    DefineUProperty(bool, bIsFinalizingExperience)
    DefineUProperty(bool, bAreZoneStatsFinalized)
    DefineUProperty(AFortTeamInfo*, PlayerTeam)
    DefineBitfieldUProperty(bHasStartedPlaying)
    DefineUFunction(OnRep_CharacterParts)

    UCustomCharacterPart*& GetCharacterPart(EFortCustomPartType PartType)
    {
        UCustomCharacterPart*& Parts = CharacterParts;
        return (&Parts)[static_cast<uint8_t>(PartType)];
    }

    void SetHeroType(UFortHero* Hero);
    void OnRep_CharacterParts();
    int32_t GetStat(EFortReplicatedStat::Type StatType, bool bCampaign);

    static ptrdiff_t FinalizingExperienceOffset();

    static void Hook();

private:
    static inline void (*SetHeroTypeOG)(AFortPlayerState* This, UFortHero* Hero) = nullptr;
    static void SetHeroTypeHK(AFortPlayerState* This, UFortHero* Hero);
};
