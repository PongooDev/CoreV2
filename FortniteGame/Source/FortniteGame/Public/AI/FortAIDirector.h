#pragma once

#include "Containers/Array.h"
#include "Containers/UnrealString.h"
#include "Engine/CurveTable.h"
#include "EnvironmentQuery/EnvQueryTypes.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "Math/Color.h"
#include "Missions/FortObjectiveBase.h"
#include "StructArray.h"
#include "Templates/SubclassOf.h"

class UEnvQuery;
class UFortAIEncounterInfo;

class EFortCombatFactors
{
    DefineUnrealEnum(EFortCombatFactors, FortniteGame)
};

class EFortAIDirectorFactor
{
    DefineUnrealEnum(EFortAIDirectorFactor, FortniteGame)
};

class EFortFactorContributionType
{
    DefineUnrealEnum(EFortFactorContributionType, FortniteGame)
};

class EFortAIUtility
{
    DefineUnrealEnum(EFortAIUtility, FortniteGame)
};

struct FIntensityContribution
{
    DefineUnrealStruct(FIntensityContribution, FortniteGame)

public:
    DefineUProperty(EFortCombatFactors, CombatFactor)
    DefineUProperty(EFortAIDirectorFactor, ContributingAIDirectorFactor)
    DefineUProperty(float, MaxContribution)
    DefineUProperty(bool, bModifyContributionByCompletionPercentage)
    DefineUProperty(bool, bModifyByNumberOfCriticalEncounterGoals)
};

struct FIntensityData
{
    DefineUnrealStruct(FIntensityData, FortniteGame)

public:
    DefineUStructArrayProperty(FIntensityContribution, ContributingFactors)
    DefineUProperty(float, ContributionsTotal)
    DefineUProperty(TArray<UObject*>, ExceptionEditModes)
};

struct FFortAIEncounterPIDControllerSettings
{
    DefineUnrealStruct(FFortAIEncounterPIDControllerSettings, FortniteGame)

public:
    DefineUStructProperty(FCurveTableRowHandle, ProportionalGain)
    DefineUStructProperty(FCurveTableRowHandle, IntegralGain)
    DefineUStructProperty(FCurveTableRowHandle, DerivativeGain)
};

struct FUtilityContribution
{
    DefineUnrealStruct(FUtilityContribution, FortniteGame)

public:
    DefineUProperty(EFortCombatFactors, ContributingFactor)
    DefineUProperty(EFortAIDirectorFactor, ContributingAIDirectorFactor)
    DefineUProperty(float, MaxContribution)
    DefineUProperty(EFortFactorContributionType, ContributionType)
};

struct FUtilityData
{
    DefineUnrealStruct(FUtilityData, FortniteGame)

public:
    DefineUStructArrayProperty(FUtilityContribution, ContributingFactors)
    DefineUProperty(bool, bApplyRecentSelectionPenalty)
    DefineUProperty(float, RecentlySelectedPenaltyPercentage)
    DefineUProperty(float, PenaltyFallOffRate)
    DefineUProperty(FString, DebugGraphName)
    DefineUProperty(FLinearColor, DebugGraphColor)
};

struct FFortPlayerPerformanceEstimateSettings
{
    DefineUnrealStruct(FFortPlayerPerformanceEstimateSettings, FortniteGame)

public:
    DefineUStructProperty(FCurveTableRowHandle, PlayerPerformanceEstimateTransformMin)
    DefineUStructProperty(FCurveTableRowHandle, PlayerPerformanceEstimateTransformOrigin)
    DefineUStructProperty(FCurveTableRowHandle, PlayerPerformanceEstimateTransformMax)
};

struct FFortAIEncounterSpawnGroupCap
{
    DefineUnrealStruct(FFortAIEncounterSpawnGroupCap, FortniteGame)

public:
    DefineUStructProperty(FCurveTableRowHandle, MinSpawnGroupNumberCap)
    DefineUStructProperty(FCurveTableRowHandle, MaxSpawnGroupNumberCap)
};

struct FFortAIEncounterSpawnGroupCapsCategory
{
    DefineUnrealStruct(FFortAIEncounterSpawnGroupCapsCategory, FortniteGame)

public:
    DefineUProperty(FGameplayTagQuery, TagQuery)
    DefineUProperty(bool, bApplyGroupPopulationCurveToCategoryMax)
    DefineUStructProperty(FCurveTableRowHandle, SpawnGroupAvailabilityDelaySeconds)
    DefineUStructArrayProperty(FFortAIEncounterSpawnGroupCap, SpawnGroupCapsPerPlayerCount)
    DefineUProperty(float, InitialSpawnGroupAvailabilityTime)
    DefineUProperty(float, NumActiveCategorySpawnGroups)
    DefineUProperty(int32_t, NumSpawnGroupAvailable)
};

struct FFortAIEncounterSpawnGroupCapsProfile
{
    DefineUnrealStruct(FFortAIEncounterSpawnGroupCapsProfile, FortniteGame)

public:
    DefineUProperty(FGameplayTagContainer, EncounterTypeTags)
    DefineUStructArrayProperty(FFortAIEncounterSpawnGroupCapsCategory, PopulationCategories)
};

struct FFortAIEncounterSpawnPointsProfile
{
    DefineUnrealStruct(FFortAIEncounterSpawnPointsProfile, FortniteGame)

public:
    DefineUProperty(FGameplayTagContainer, EncounterTypeTags)
    DefineUStructArrayProperty(FCurveTableRowHandle, MaxSpawnPointsPerPlayerCount)
    DefineUStructArrayProperty(FCurveTableRowHandle, MinSpawnPointsPerPlayerCount)
};

struct FFortAIEncounterPawnDifficultyLevelModifier
{
    DefineUnrealStruct(FFortAIEncounterPawnDifficultyLevelModifier, FortniteGame)

public:
    DefineUProperty(FGameplayTagQuery, EncounterTagRequirementsQuery)
    DefineUStructProperty(FCurveTableRowHandle, DifficultyLevelModifierCurve)
};

class UFortAISpawnGroupUpgrade : public UObject
{
    DefineUnrealClass(UFortAISpawnGroupUpgrade, FortniteGame)
};

class UFortAISpawnGroupUpgradeProbabilities : public UObject
{
    DefineUnrealClass(UFortAISpawnGroupUpgradeProbabilities, FortniteGame)
};

class UFortAISpawnGroupCapsCategorySet : public UObject
{
    DefineUnrealClass(UFortAISpawnGroupCapsCategorySet, FortniteGame)
};

// From 1.10 on, UFortAIEncounterInfo::AvailableUpgrades holds these in place of 1.2's FFortAISpawnGroupUpgrade.
struct FFortAISpawnGroupUpgradeData
{
    DefineUnrealStruct(FFortAISpawnGroupUpgradeData, FortniteGame)

public:
    DefineUProperty(TSubclassOf<UFortAISpawnGroupUpgrade>, SpawnGroupUpgrade)
    DefineUProperty(TSubclassOf<UFortAISpawnGroupUpgradeProbabilities>, UpgradeProbabilities)
    DefineUProperty(TSubclassOf<UFortAISpawnGroupCapsCategorySet>, SpawnGroupCapsCategories)
    DefineUProperty(FGameplayTagQuery, EncounterTagRequirementsQuery)
};

struct FEncounterEnvironmentQueryInfo
{
    DefineUnrealStruct(FEncounterEnvironmentQueryInfo, FortniteGame)

public:
    DefineUProperty(UEnvQuery*, EnvironmentQuery)
    DefineUStructArrayProperty(FEnvNamedValue, QueryParams)
    DefineUProperty(bool, bIsDirectional)
};

struct FFortEncounterPawnNumberCaps
{
    DefineUnrealStruct(FFortEncounterPawnNumberCaps, FortniteGame)

public:
    DefineUProperty(bool, bApplyPawnNumberCaps)
};

class AFortAIDirector : public AActor
{
    DefineUnrealClass(AFortAIDirector, FortniteGame)

public:
    DefineUStructProperty(FCurveTableRowHandle, MaxAliveCurve)
    DefineUStructProperty(FCurveTableRowHandle, MinAliveCurve)
    DefineUStructProperty(FIntensityData, IntensityInfo)
    DefineUStructProperty(FFortAIEncounterPIDControllerSettings, PIDControllerSettings)
    // UtilityContributionData[EFortAIUtility::MAX], one per utility: element 0, the rest UtilityContributionDataAt.
    DefineUStructProperty(FUtilityData, UtilityContributionData)
    DefineUStructProperty(FFortPlayerPerformanceEstimateSettings, PlayerPerformanceEstimateSettings)
    DefineUProperty(TArray<UFortAIEncounterInfo*>, ActiveEncounters)
    DefineUProperty(UFortAIEncounterInfo*, ActiveDefaultEncounter)
    DefineUProperty(UFortAIEncounterInfo*, ActiveDummyDebugEncounter)

protected:
    DefineUProperty(float, BurstSpawnThreatVisualsEndDelay)
    DefineUStructArrayProperty(FFortAIEncounterSpawnGroupCapsProfile, EncounterSpawnGroupCapSettings)
    DefineUStructArrayProperty(FFortAIEncounterSpawnPointsProfile, EncounterSpawnPointsSettings)
    DefineUStructArrayProperty(FFortAIEncounterPawnDifficultyLevelModifier, EncounterPawnDifficultyLevelModifiers)
    DefineUStructArrayProperty(FFortAISpawnGroupUpgradeData, SpawnGroupUpgrades)
    // Fixed arrays of four: element 0 here, the rest through the element size.
    DefineUProperty(float, DiscreteEncounterUtilityDesireMappings)
    DefineUProperty(EFortAIUtility, InitialDynamicUtilities)
    DefineUStructProperty(FEncounterEnvironmentQueryInfo, SpawnLocationPlacementQueries)
    DefineUProperty(UObject*, DataTrackingSettings)
    DefineUStructProperty(FFortEncounterPawnNumberCaps, DefaultEncounterPawnCaps)
    DefineUProperty(FGameplayTagContainer, AmbientThreatPreferredPlacementActorTags)
    DefineUProperty(FGameplayTagContainer, AmbientThreatRequiredPlacementActorTags)

public:
    DefineUProperty(float, EncounterPawnSpawnInterval)
    DefineUProperty(TSubclassOf<UFortAIEncounterInfo>, DummyDebugEncounter)
    DefineUProperty(TSubclassOf<UFortAIEncounterInfo>, BaseEncounterClass)
    DefineUProperty(int32_t, MaxActiveAlive)
    DefineUProperty(int32_t, NumWorldSubdivides)
    DefineUProperty(float, MinAISpawnDistanceFromPlayers)
    DefineUProperty(float, MaxNormalLODDistanceToPlayer)
    DefineUProperty(float, AIRelevantDistanceToPlayer)
    DefineUProperty(float, EncounterRelevantDistanceToPlayer)

    static AFortAIDirector* GetCurrent(const UObject* WorldContextObject);

    void StopEncounter(UFortAIEncounterInfo* EncounterToStop, EFortObjectiveStatus EncounterStopReason, bool bForceKillAI = false, bool bBroadcastStop = true);

    // Stands in for /Game/AIDirector/AIDirector_Fortnite, which 1.10's paks don't ship: sets what its class defaults set.
    void ApplyAIDirectorFortniteDefaults();

private:
    FUtilityData& UtilityContributionDataAt(int32_t Index) const;
};
