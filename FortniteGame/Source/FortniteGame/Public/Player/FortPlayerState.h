#pragma once

#include "Customization/CustomCharacterPart.h"
#include "GameFramework/PlayerState.h"
#include "StructArray.h"

class AFortPlayerPawn;
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

// From 1.7.2.
class EFortIsFinalXpUpdate
{
    DefineUnrealEnum(EFortIsFinalXpUpdate, FortniteGame)
    DefineUEnumValue(Uninitialized)
    DefineUEnumValue(NotFinal)
    DefineUEnumValue(Final)
};

struct FFortExperienceDelta
{
    DefineUnrealStruct(FFortExperienceDelta, FortniteGame)
    DefineUProperty(int32_t, Level)
    DefineUProperty(int32_t, Xp)
    DefineUProperty(int32_t, BaseXPEarned)
    DefineUProperty(int32_t, BonusXPEarned)
    DefineUProperty(int32_t, BoostXPEarned)
    DefineUProperty(int32_t, BoostXPMissed)
    DefineUProperty(int32_t, RestXPEarned)
    DefineUProperty(int32_t, GroupBoostXPEarned)
    DefineUProperty(EFortIsFinalXpUpdate, IsFinalXpUpdate)

    // Whether deltas are placeholders that an earnScore notification fills in (1.7.2 on), not added by it (1.1, 1.2).
    static bool HasFinalXpUpdate() { return Member_IsFinalXpUpdate.IsAvailable(); }

    // What the results screen and the earnScore handler call a fully initialized delta (1.8 0x2F1AD90): every
    // number set (a placeholder starts at -1) and its finality known.
    bool IsInitialized() const
    {
        return Level >= 0 && Xp >= 0 && BaseXPEarned >= 0 && BonusXPEarned >= 0 && BoostXPEarned >= 0 && BoostXPMissed >= 0 && RestXPEarned >= 0 && GroupBoostXPEarned >= 0 && IsFinalXpUpdate != EFortIsFinalXpUpdate::GetUninitialized();
    }
};

// One EFortReplicatedStat's count and the score it earned (AFortPlayerState::ReplicatedStats_Campaign and _Zone, one
// per stat type).
struct FReplicatedStatValues
{
    DefineUnrealStruct(FReplicatedStatValues, FortniteGame)
    DefineUProperty(int32_t, StatValue)
    DefineUProperty(int32_t, ScoreValue)
};

class AFortPlayerState : public APlayerState
{
    DefineUnrealClass(AFortPlayerState, FortniteGame)

public:
    DefineUStructProperty(FReplicatedStatValues, ReplicatedStats_Campaign)
    DefineUStructProperty(FReplicatedStatValues, ReplicatedStats_Zone)
    DefineUProperty(UAbilitySystemComponent*, AbilitySystemComponent)
    DefineUProperty(uint8_t, CharacterGender)
    DefineUProperty(UCustomCharacterPart*, CharacterParts)
    DefineUProperty(int32_t, WorldPlayerId)
    DefineUProperty(bool, bIsFinalizingExperience)
    DefineUStructArrayProperty(FFortExperienceDelta, ExperienceDeltas)
    DefineUProperty(bool, bAreZoneStatsFinalized)
    DefineUProperty(AFortTeamInfo*, PlayerTeam)
    DefineUStructProperty(FUniqueNetIdRepl, PartyOwnerUniqueId)
    DefineBitfieldUProperty(bHasStartedPlaying)
    DefineUFunction(OnRep_CharacterParts)
    DefineUFunction(GetCurrentPawn)

    UCustomCharacterPart*& GetCharacterPart(EFortCustomPartType PartType)
    {
        UCustomCharacterPart*& Parts = CharacterParts;
        return (&Parts)[static_cast<uint8_t>(PartType)];
    }

    void SetHeroType(UFortHero* Hero);
    // Whether SetHeroType can run here (the server found the native).
    static bool CanSetHeroType() { return SetHeroTypeOG != nullptr; }
    // Epic's: on the server, chooses the player state's character parts, gender and body type for its controller's hero
    // (the one whose id it holds), then from 2.4.2 the athena profile's favorite back bling, and gives the pawn its parts.
    void ApplyCharacterCustomization(AFortPlayerPawn* Pawn);
    // Whether the build has ApplyCharacterCustomization (1.9 on).
    static bool CanApplyCharacterCustomization();
    void OnRep_CharacterParts();
    // The player pawn whose player state this is (1.9: AFortPlayerState::GetCurrentPawn), or null.
    AFortPlayerPawn* GetCurrentPawn();
    // The score the stat earned (its ScoreValue: 2.1 0x8E80E0 reads ReplicatedStats_Campaign or _Zone's).
    int32_t GetStat(EFortReplicatedStat StatType, bool bCampaign);
    // The stat's count (its StatValue), which AFortPlayerStateZone::ModifyStat adds StatDelta to (2.1 0x8ED110), 0 when
    // the build doesn't reflect the stats.
    int32_t GetStatValue(EFortReplicatedStat StatType, bool bCampaign);

    // bIsFinalizingExperience is gone from 1.7.2, which finalizes through FFortExperienceDelta::IsFinalXpUpdate.
    static bool HasFinalizingExperience();
    // Takes out the experience deltas still waiting for an earnScore notification, returning how many there were.
    int32_t RemoveUnscoredExperienceDeltas();

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
