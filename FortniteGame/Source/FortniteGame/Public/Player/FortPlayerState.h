#pragma once

#include "Customization/CustomCharacterPart.h"
#include "GameFramework/PlayerState.h"
#include "StructArray.h"

class AFortTeamInfo;
class UAbilitySystemComponent;
class UFortHero;
class UFortItemDefinition;
struct FVector;

class EFortReplicatedStat
{
    DefineUnrealEnum(EFortReplicatedStat, FortniteGame)
    DefineUEnumValue(MonsterKills)
    DefineUEnumValue(MonsterDamagePoints)
    DefineUEnumValue(PlayerKills)
    DefineUEnumValue(WoodGathered)
    DefineUEnumValue(StoneGathered)
    DefineUEnumValue(MetalGathered)
    DefineUEnumValue(Deaths)
    DefineUEnumValue(BuildingsBuilt)
    DefineUEnumValue(BuildingsBuilt_Wood)
    DefineUEnumValue(BuildingsBuilt_Stone)
    DefineUEnumValue(BuildingsBuilt_Metal)
    DefineUEnumValue(BuildingsUpgraded_Wood2)
    DefineUEnumValue(BuildingsUpgraded_Wood3)
    DefineUEnumValue(BuildingsUpgraded_Stone2)
    DefineUEnumValue(BuildingsUpgraded_Stone3)
    DefineUEnumValue(BuildingsUpgraded_Metal2)
    DefineUEnumValue(BuildingsUpgraded_Metal3)
    DefineUEnumValue(BuildingsDestroyed)
    DefineUEnumValue(Repair_Wood)
    DefineUEnumValue(Repair_Stone)
    DefineUEnumValue(Repair_Metal)
    DefineUEnumValue(FlagsCaptured)
    DefineUEnumValue(FlagsReturned)
    DefineUEnumValue(ContainersLooted)
    DefineUEnumValue(CraftingPoints)
    DefineUEnumValue(TrapPlacementPoints)
    DefineUEnumValue(TrapActivationPoints)
    DefineUEnumValue(TotalScore)
    DefineUEnumValue(OldTotalScore)
    DefineUEnumValue(CombatScore)
    DefineUEnumValue(BuildingScore)
    DefineUEnumValue(UtilityScore)
    DefineUEnumValue(BadgesScore)
    DefineUEnumValue(None)
    DefineUEnumValue(MAX)
};

class AFortPlayerState : public APlayerState
{
    DefineUnrealClass(AFortPlayerState, FortniteGame)

public:
    DefineUProperty(UAbilitySystemComponent*, AbilitySystemComponent)
    DefineUProperty(uint8_t, CharacterGender)
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
    int32_t GetStat(EFortReplicatedStat StatType, bool bCampaign);

    // bIsFinalizingExperience is gone from 1.7.2, which finalizes through FFortExperienceDelta::IsFinalXpUpdate.
    static bool HasFinalizingExperience();

    static void Hook();

private:
    static inline void (*SetHeroTypeOG)(AFortPlayerState* This, UFortHero* Hero) = nullptr;
    static void SetHeroTypeHK(AFortPlayerState* This, UFortHero* Hero);
};

struct FAccumulatedItemEntry
{
    DefineUnrealStruct(FAccumulatedItemEntry, FortniteGame)
    DefineUProperty(UFortItemDefinition*, ItemDefinition)
    DefineUProperty(int32_t, Quantity)
};

class AFortPlayerStateZone : public AFortPlayerState
{
    DefineUnrealClass(AFortPlayerStateZone, FortniteGame)

public:
    DefineUStructArrayProperty(FAccumulatedItemEntry, AccumulatedItems)
    DefineUFunction(OnRep_AccumulatedItems)

    void AddAccumulatedItem(UFortItemDefinition* ItemDefinition, int32_t Delta);
    void ModifyStat(EFortReplicatedStat StatType, int32_t StatDelta, int32_t ScoreDelta, const FVector& Location);
    void OnRep_AccumulatedItems();
};
