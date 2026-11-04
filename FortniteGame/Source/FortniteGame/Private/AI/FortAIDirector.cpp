#include "pch.h"
#include "AI/FortAIDirector.h"
#include "AI/FortAIEncounterInfo.h"
#include "Engine/World.h"
#include "EnvironmentQuery/EnvQuery.h"
#include "FortGameModeZone.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "Templates/Casts.h"
#include "UObject/UObjectGlobals.h"

#include <initializer_list>
#include <optional>
#include <string>
#include <vector>

DEFINE_LOG_CATEGORY_STATIC(LogFortAIDirector, Log, All);

namespace
{
    // Default__AIDirector_Fortnite_C, the class defaults of /Game/AIDirector/AIDirector_Fortnite as 1.9.1 (CL-3775276)
    // cooks them: every property it sets over AFortAIDirector's. Its graph is empty (OnPeakStarted, OnFadeStarted,
    // OnRestStarted and OnRampStarted do nothing) and its construction script adds only the default scene root, so these
    // values are all of it. Left out: its own Blueprint variables (the PatrolWard and Guardian ones, which no native code
    // reads) and its components. Every asset and curve table row named here is in 1.10's paks.
    constexpr const wchar_t* AIDirectorAliveCurvesPath = L"/Game/AIDirector/DataTables/AIDirectorAliveCurves.AIDirectorAliveCurves";
    constexpr const wchar_t* AIDirPIDPath = L"/Game/AIDirector/DataTables/AIDirPID.AIDirPID";
    constexpr const wchar_t* AIDirPPEPath = L"/Game/AIDirector/DataTables/AIDirPPE.AIDirPPE";
    constexpr const wchar_t* AIDirectorPopulationCategoryCapsPath = L"/Game/AIDirector/DataTables/AIDirectorPopulationCategoryCaps.AIDirectorPopulationCategoryCaps";
    constexpr const wchar_t* AIDirectorSpawnBudgetCurvesPath = L"/Game/AIDirector/DataTables/AIDirectorSpawnBudgetCurves.AIDirectorSpawnBudgetCurves";
    constexpr const wchar_t* GroupEnemyOffsetsPath = L"/Game/Balance/DataTables/GroupEnemyOffsets.GroupEnemyOffsets";
    // Mini-Bosses came with 1.11; a build without them has no such class.
    constexpr const wchar_t* MiniBossSpawnGroupUpgradePath = L"/Game/AIDirector/MiniBoss/SpawnGroupUpgrades/SGU_MiniBoss_1P.SGU_MiniBoss_1P_C";

    struct FIntensityContributionDefaults
    {
        const wchar_t* CombatFactor;
        const wchar_t* ContributingAIDirectorFactor;
        float MaxContribution;
        bool bModifyContributionByCompletionPercentage;
        bool bModifyByNumberOfCriticalEncounterGoals;
    };

    struct FUtilityContributionDefaults
    {
        const wchar_t* ContributingFactor;
        const wchar_t* ContributingAIDirectorFactor;
        float MaxContribution;
        const wchar_t* ContributionType;
    };

    // One UtilityContributionData element, in EFortAIUtility order; an unset optional keeps AFortAIDirector's value.
    struct FUtilityDataDefaults
    {
        std::vector<FUtilityContributionDefaults> ContributingFactors;
        std::optional<bool> bApplyRecentSelectionPenalty;
        float RecentlySelectedPenaltyPercentage;
        float PenaltyFallOffRate;
        const wchar_t* DebugGraphName;
        std::optional<FLinearColor> DebugGraphColor;
    };

    struct FSpawnGroupCapDefaults
    {
        const wchar_t* MinSpawnGroupNumberCapRow;
        const wchar_t* MaxSpawnGroupNumberCapRow;
    };

    // A population category: its query is ALL( Encounter.PopulationCategory.<Category> ), described "<Category> Population
    // Category", and it applies the group population curve to the category max. The rows are AIDirectorPopulationCategoryCaps'.
    struct FSpawnGroupCapsCategoryDefaults
    {
        const wchar_t* Category;
        const wchar_t* SpawnGroupAvailabilityDelaySecondsRow;
        std::vector<FSpawnGroupCapDefaults> SpawnGroupCapsPerPlayerCount;
    };

    struct FSpawnGroupCapsProfileDefaults
    {
        const wchar_t* EncounterTypeTag;
        std::vector<FSpawnGroupCapsCategoryDefaults> PopulationCategories;
    };

    // The rows are AIDirectorSpawnBudgetCurves'.
    struct FSpawnPointsProfileDefaults
    {
        const wchar_t* EncounterTypeTag;
        std::vector<const wchar_t*> MaxSpawnPointsPerPlayerCountRows;
        std::vector<const wchar_t*> MinSpawnPointsPerPlayerCountRows;
    };

    const FIntensityContributionDefaults IntensityContributionDefaults[] =
    {
        { L"PlayerDamageThreat", L"PlayerDamageThreat", 5.7f, false, false },
        { L"ObjectiveDamageThreat", L"ObjectiveDamageThreat", 11.9f, false, true },
    };

    const FUtilityDataDefaults UtilityDefaults[] =
    {
        { { { L"PlayerDamageThreat", L"PlayerDamageThreat", 8.0f, L"AverageValue_Inverse" }, { L"PlayerPathCost", L"PlayerPathCost", 3.0f, L"AverageValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Kill Players Melee", FLinearColor{ 0.15f, 0.0f, 0.0f, 1.0f } },
        { { { L"PlayerDamageThreat", L"PlayerDamageThreat", 10.0f, L"AverageValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Kill Players Ranged", FLinearColor{ 0.4f, 0.0f, 0.0f, 1.0f } },
        { { { L"PlayerDamageThreat", L"PlayerDamageThreat", 8.0f, L"AverageValue_Inverse" }, { L"PlayerPathCost", L"PlayerPathCost", 2.0f, L"AverageValue_Direct" } }, std::nullopt, 0.8f, 0.6f, L"Kill Players Artillery", FLinearColor{ 1.0f, 0.0f, 0.0f, 1.0f } },
        { { { L"ObjectiveDamageThreat", L"ObjectiveDamageThreat", 8.0f, L"AverageValue_Inverse" }, { L"ObjectivePathCost", L"ObjectivePathCost", 2.0f, L"AverageValue_Direct" }, { L"NearbyEnemyPresence", L"NearbyEnemyPresence", 2.0f, L"AverageValue_Direct" } }, std::nullopt, 0.8f, 0.6f, L"Destroy Buildings Melee", FLinearColor{ 0.15f, 0.09375f, 0.0375f, 1.0f } },
        { { { L"ObjectiveDamageThreat", L"ObjectiveDamageThreat", 8.0f, L"AverageValue_Inverse" }, { L"ObjectivePathCost", L"ObjectivePathCost", 2.0f, L"AverageValue_Direct" }, { L"NearbyEnemyPresence", L"NearbyEnemyPresence", 2.0f, L"AverageValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Destroy Buildings Ranged", FLinearColor{ 0.4f, 0.25f, 0.1f, 1.0f } },
        { { { L"ObjectiveDamageThreat", L"ObjectiveDamageThreat", 8.0f, L"AverageValue_Inverse" }, { L"NearbyEnemyPresence", L"NearbyEnemyPresence", 2.0f, L"AverageValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Destroy Buildings Artillery", FLinearColor{ 1.0f, 0.625f, 0.25f, 1.0f } },
        { { { L"TrapsEffective", L"TrapsEffective", 10.0f, L"AverageValue_Direct" } }, std::nullopt, 0.8f, 0.6f, L"Destroy Traps", FLinearColor{ 1.0f, 1.0f, 0.0f, 1.0f } },
        { { { L"Boredom", L"Boredom", 7.0f, L"AverageValue_Direct" }, { L"PlayerDamageThreat", L"PlayerDamageThreat", 1.5f, L"AverageValue_Inverse" }, { L"ObjectiveDamageThreat", L"ObjectiveDamageThreat", 1.5f, L"AverageValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Distract: Tank", std::nullopt },
        { { { L"ObjectiveDamageThreat", L"ObjectiveDamageThreat", 2.0f, L"AverageValue_Inverse" }, { L"ObjectivePathCost", L"ObjectivePathCost", 8.0f, L"AverageValue_Direct" } }, std::nullopt, 0.8f, 0.6f, L"Distract: Infiltrate", FLinearColor{ 0.5f, 0.0f, 1.0f, 1.0f } },
        { { { L"ObjectivePathCost", L"ObjectivePathCost", 10.0f, L"AverageValue_Inverse" }, { L"ObjectivePathCost", L"ObjectivePathCost", 3.0f, L"AverageValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Distract: Mob", FLinearColor{ 0.4f, 0.4f, 0.4f, 1.0f } },
        { { { L"PlayerDamageThreat", L"PlayerDamageThreat", 0.0f, L"AverageValue_Inverse" }, { L"ObjectiveDamageThreat", L"ObjectiveDamageThreat", 0.0f, L"AverageValue_Inverse" }, { L"OffensiveResources", L"OffensiveResources", 0.0f, L"CurrentValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Distract:Support", FLinearColor{ 0.1f, 0.1f, 1.0f, 1.0f } },
        { { { L"PlayerDamageThreat", L"PlayerDamageThreat", 0.0f, L"AverageValue_Inverse" }, { L"PlayerMovement", L"PlayerMovement", 0.0f, L"AverageValue_Inverse" }, { L"OffensiveResources", L"OffensiveResources", 0.0f, L"CurrentValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Distract: Kiting", FLinearColor{ 0.0f, 1.0f, 1.0f, 1.0f } },
        { { { L"PlayerDamageThreat", L"PlayerDamageThreat", 8.0f, L"AverageValue_Inverse" }, { L"PlayerMovement", L"PlayerMovement", 2.0f, L"AverageValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Distract: Area of Denial", FLinearColor{ 0.0f, 1.0f, 0.0f, 1.0f } },
        { { { L"TrapsEffective", L"TrapsEffective", 0.0f, L"AverageValue_Direct" }, { L"ObjectivePathCost", L"ObjectivePathCost", 0.0f, L"AverageValue_Inverse" } }, std::nullopt, 0.8f, 0.6f, L"Distract: Disable Traps", FLinearColor{ 0.4f, 0.4f, 0.0f, 1.0f } },
        { { { L"PlayerDamageThreat", L"PlayerDamageThreat", 0.0f, L"AverageValue_Inverse" }, { L"Boredom", L"Max_None", 0.0f, L"AverageValue_Direct" } }, false, 0.8f, 0.6f, L"Distract: Dormant", FLinearColor{ 0.508475f, 0.107728f, 0.133545f, 1.0f } },
        { { { L"PlayerDamageThreat", L"PlayerDamageThreat", 8.0f, L"AverageValue_Inverse" }, { L"Boredom", L"Boredom", 2.0f, L"AverageValue_Direct" } }, std::nullopt, 0.8f, 0.6f, L"Assassin", FLinearColor{ 1.0f, 0.3f, 0.0f, 1.0f } },
    };
    const FSpawnGroupCapsProfileDefaults SpawnGroupCapsDefaults[] =
    {
        { L"Encounter.Type.PvE.Primary.Standard",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_1_Min", L"PvE_Primary_Standard_Basic_1_Max" }, { L"PvE_Primary_Standard_Basic_2_Min", L"PvE_Primary_Standard_Basic_2_Max" }, { L"PvE_Primary_Standard_Basic_3_Min", L"PvE_Primary_Standard_Basic_3_Max" }, { L"PvE_Primary_Standard_Basic_4_Min", L"PvE_Primary_Standard_Basic_4_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_1_Min", L"PvE_Primary_Standard_Common_1_Max" }, { L"PvE_Primary_Standard_Common_2_Min", L"PvE_Primary_Standard_Common_2_Max" }, { L"PvE_Primary_Standard_Common_3_Min", L"PvE_Primary_Standard_Common_3_Max" }, { L"PvE_Primary_Standard_Common_4_Min", L"PvE_Primary_Standard_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_1_Min", L"PvE_Primary_Standard_Uncommon_1_Max" }, { L"PvE_Primary_Standard_Uncommon_2_Min", L"PvE_Primary_Standard_Uncommon_2_Max" }, { L"PvE_Primary_Standard_Uncommon_3_Min", L"PvE_Primary_Standard_Uncommon_3_Max" }, { L"PvE_Primary_Standard_Uncommon_4_Min", L"PvE_Primary_Standard_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_1_Min", L"PvE_Primary_Standard_Rare_1_Max" }, { L"PvE_Primary_Standard_Rare_2_Min", L"PvE_Primary_Standard_Rare_2_Max" }, { L"PvE_Primary_Standard_Rare_3_Min", L"PvE_Primary_Standard_Rare_3_Max" }, { L"PvE_Primary_Standard_Rare_4_Min", L"PvE_Primary_Standard_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.NonScaling.4Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_4_Min", L"PvE_Primary_Standard_Basic_4_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_4_Min", L"PvE_Primary_Standard_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_4_Min", L"PvE_Primary_Standard_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_4_Min", L"PvE_Primary_Standard_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT01.4Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_4_Min", L"PvE_Primary_Standard_Basic_4_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_4_Min", L"PvE_Primary_Standard_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_4_Min", L"PvE_Primary_Standard_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_4_Min", L"PvE_Primary_Standard_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT02.4Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_4_Min", L"PvE_Primary_Standard_Basic_4_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_4_Min", L"PvE_Primary_Standard_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_4_Min", L"PvE_Primary_Standard_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_4_Min", L"PvE_Primary_Standard_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT03.4Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_4_Min", L"PvE_Primary_Standard_Basic_4_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_4_Min", L"PvE_Primary_Standard_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_4_Min", L"PvE_Primary_Standard_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_4_Min", L"PvE_Primary_Standard_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT04.4Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_4_Min", L"PvE_Primary_Standard_Basic_4_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_4_Min", L"PvE_Primary_Standard_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_4_Min", L"PvE_Primary_Standard_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_4_Min", L"PvE_Primary_Standard_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.Group.4Player",
          {
            { L"Basic", L"PvE_Primary_Group_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Group_Basic_4_Min", L"PvE_Primary_Group_Basic_4_Max" } } },
            { L"Common", L"PvE_Primary_Group_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Group_Common_4_Min", L"PvE_Primary_Group_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Primary_Group_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Group_Uncommon_4_Min", L"PvE_Primary_Group_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Primary_Group_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Group_Rare_4_Min", L"PvE_Primary_Group_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.NonScaling.3Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_3_Min", L"PvE_Primary_Standard_Basic_3_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_3_Min", L"PvE_Primary_Standard_Common_3_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_3_Min", L"PvE_Primary_Standard_Uncommon_3_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_3_Min", L"PvE_Primary_Standard_Rare_3_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT01.3Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_3_Min", L"PvE_Primary_Standard_Basic_3_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_3_Min", L"PvE_Primary_Standard_Common_3_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_3_Min", L"PvE_Primary_Standard_Uncommon_3_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_3_Min", L"PvE_Primary_Standard_Rare_3_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT02.3Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_3_Min", L"PvE_Primary_Standard_Basic_3_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_3_Min", L"PvE_Primary_Standard_Common_3_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_3_Min", L"PvE_Primary_Standard_Uncommon_3_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_3_Min", L"PvE_Primary_Standard_Rare_3_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT03.3Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_3_Min", L"PvE_Primary_Standard_Basic_3_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_3_Min", L"PvE_Primary_Standard_Common_3_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_3_Min", L"PvE_Primary_Standard_Uncommon_3_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_3_Min", L"PvE_Primary_Standard_Rare_3_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT04.3Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_3_Min", L"PvE_Primary_Standard_Basic_3_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_3_Min", L"PvE_Primary_Standard_Common_3_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_3_Min", L"PvE_Primary_Standard_Uncommon_3_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_3_Min", L"PvE_Primary_Standard_Rare_3_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.NonScaling.2Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_2_Min", L"PvE_Primary_Standard_Basic_2_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_2_Min", L"PvE_Primary_Standard_Common_2_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_2_Min", L"PvE_Primary_Standard_Uncommon_2_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_2_Min", L"PvE_Primary_Standard_Rare_2_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT01.2Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_2_Min", L"PvE_Primary_Standard_Basic_2_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_2_Min", L"PvE_Primary_Standard_Common_2_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_2_Min", L"PvE_Primary_Standard_Uncommon_2_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_2_Min", L"PvE_Primary_Standard_Rare_2_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT02.2Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_2_Min", L"PvE_Primary_Standard_Basic_2_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_2_Min", L"PvE_Primary_Standard_Common_2_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_2_Min", L"PvE_Primary_Standard_Uncommon_2_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_2_Min", L"PvE_Primary_Standard_Rare_2_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT03.2Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_2_Min", L"PvE_Primary_Standard_Basic_2_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_2_Min", L"PvE_Primary_Standard_Common_2_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_2_Min", L"PvE_Primary_Standard_Uncommon_2_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_2_Min", L"PvE_Primary_Standard_Rare_2_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT04.2Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_2_Min", L"PvE_Primary_Standard_Basic_2_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_2_Min", L"PvE_Primary_Standard_Common_2_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_2_Min", L"PvE_Primary_Standard_Uncommon_2_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_2_Min", L"PvE_Primary_Standard_Rare_2_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.NonScaling.1Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_1_Min", L"PvE_Primary_Standard_Basic_1_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_1_Min", L"PvE_Primary_Standard_Common_1_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_1_Min", L"PvE_Primary_Standard_Uncommon_1_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_1_Min", L"PvE_Primary_Standard_Rare_1_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT01.1Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_1_Min", L"PvE_Primary_Standard_Basic_1_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_1_Min", L"PvE_Primary_Standard_Common_1_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_1_Min", L"PvE_Primary_Standard_Uncommon_1_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_1_Min", L"PvE_Primary_Standard_Rare_1_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT02.1Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_1_Min", L"PvE_Primary_Standard_Basic_1_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_1_Min", L"PvE_Primary_Standard_Common_1_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_1_Min", L"PvE_Primary_Standard_Uncommon_1_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_1_Min", L"PvE_Primary_Standard_Rare_1_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT03.1Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_1_Min", L"PvE_Primary_Standard_Basic_1_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_1_Min", L"PvE_Primary_Standard_Common_1_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_1_Min", L"PvE_Primary_Standard_Uncommon_1_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_1_Min", L"PvE_Primary_Standard_Rare_1_Max" } } },
          } },
        { L"Encounter.Type.PvE.Primary.OutpostT04.1Player",
          {
            { L"Basic", L"PvE_Primary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Basic_1_Min", L"PvE_Primary_Standard_Basic_1_Max" } } },
            { L"Common", L"PvE_Primary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Common_1_Min", L"PvE_Primary_Standard_Common_1_Max" } } },
            { L"Uncommon", L"PvE_Primary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Uncommon_1_Min", L"PvE_Primary_Standard_Uncommon_1_Max" } } },
            { L"Rare", L"PvE_Primary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Primary_Standard_Rare_1_Min", L"PvE_Primary_Standard_Rare_1_Max" } } },
          } },
        { L"Encounter.Type.PvE.Secondary.Standard",
          {
            { L"Basic", L"PvE_Secondary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Basic_1_Min", L"PvE_Secondary_Standard_Basic_1_Max" }, { L"PvE_Secondary_Standard_Basic_2_Min", L"PvE_Secondary_Standard_Basic_2_Max" }, { L"PvE_Secondary_Standard_Basic_3_Min", L"PvE_Secondary_Standard_Basic_3_Max" }, { L"PvE_Secondary_Standard_Basic_4_Min", L"PvE_Secondary_Standard_Basic_4_Max" } } },
            { L"Common", L"PvE_Secondary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Common_1_Min", L"PvE_Secondary_Standard_Common_1_Max" }, { L"PvE_Secondary_Standard_Common_2_Min", L"PvE_Secondary_Standard_Common_2_Max" }, { L"PvE_Secondary_Standard_Common_3_Min", L"PvE_Secondary_Standard_Common_3_Max" }, { L"PvE_Secondary_Standard_Common_4_Min", L"PvE_Secondary_Standard_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Secondary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Uncommon_1_Min", L"PvE_Secondary_Standard_Uncommon_1_Max" }, { L"PvE_Secondary_Standard_Uncommon_2_Min", L"PvE_Secondary_Standard_Uncommon_2_Max" }, { L"PvE_Secondary_Standard_Uncommon_3_Min", L"PvE_Secondary_Standard_Uncommon_3_Max" }, { L"PvE_Secondary_Standard_Uncommon_4_Min", L"PvE_Secondary_Standard_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Secondary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Rare_1_Min", L"PvE_Secondary_Standard_Rare_1_Max" }, { L"PvE_Secondary_Standard_Rare_2_Min", L"PvE_Secondary_Standard_Rare_2_Max" }, { L"PvE_Secondary_Standard_Rare_3_Min", L"PvE_Secondary_Standard_Rare_3_Max" }, { L"PvE_Secondary_Standard_Rare_4_Min", L"PvE_Secondary_Standard_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Secondary.NonScaling.4Player",
          {
            { L"Basic", L"PvE_Secondary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Basic_4_Min", L"PvE_Secondary_Standard_Basic_4_Max" } } },
            { L"Common", L"PvE_Secondary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Common_4_Min", L"PvE_Secondary_Standard_Common_4_Max" } } },
            { L"Uncommon", L"PvE_Secondary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Uncommon_4_Min", L"PvE_Secondary_Standard_Uncommon_4_Max" } } },
            { L"Rare", L"PvE_Secondary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Rare_4_Min", L"PvE_Secondary_Standard_Rare_4_Max" } } },
          } },
        { L"Encounter.Type.PvE.Secondary.NonScaling.3Player",
          {
            { L"Basic", L"PvE_Secondary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Basic_3_Min", L"PvE_Secondary_Standard_Basic_3_Max" } } },
            { L"Common", L"PvE_Secondary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Common_3_Min", L"PvE_Secondary_Standard_Common_3_Max" } } },
            { L"Uncommon", L"PvE_Secondary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Uncommon_3_Min", L"PvE_Secondary_Standard_Uncommon_3_Max" } } },
            { L"Rare", L"PvE_Secondary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Rare_3_Min", L"PvE_Secondary_Standard_Rare_3_Max" } } },
          } },
        { L"Encounter.Type.PvE.Secondary.NonScaling.2Player",
          {
            { L"Basic", L"PvE_Secondary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Basic_2_Min", L"PvE_Secondary_Standard_Basic_2_Max" } } },
            { L"Common", L"PvE_Secondary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Common_2_Min", L"PvE_Secondary_Standard_Common_2_Max" } } },
            { L"Uncommon", L"PvE_Secondary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Uncommon_2_Min", L"PvE_Secondary_Standard_Uncommon_2_Max" } } },
            { L"Rare", L"PvE_Secondary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Rare_2_Min", L"PvE_Secondary_Standard_Rare_2_Max" } } },
          } },
        { L"Encounter.Type.PvE.Secondary.NonScaling.1Player",
          {
            { L"Basic", L"PvE_Secondary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Basic_1_Min", L"PvE_Secondary_Standard_Basic_1_Max" } } },
            { L"Common", L"PvE_Secondary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Common_1_Min", L"PvE_Secondary_Standard_Common_1_Max" } } },
            { L"Uncommon", L"PvE_Secondary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Uncommon_1_Min", L"PvE_Secondary_Standard_Uncommon_1_Max" } } },
            { L"Rare", L"PvE_Secondary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Secondary_Standard_Rare_1_Min", L"PvE_Secondary_Standard_Rare_1_Max" } } },
          } },
        { L"Encounter.Type.PvE.Tertiary.Standard",
          {
            { L"Basic", L"PvE_Tertiary_Standard_Basic_AllPlayers_RespawnTime", { { L"PvE_Tertiary_Standard_Basic_1_Min", L"PvE_Tertiary_Standard_Basic_1_Max" }, { L"PvE_Tertiary_Standard_Basic_1_Min", L"PvE_Tertiary_Standard_Basic_1_Max" }, { L"PvE_Tertiary_Standard_Basic_1_Min", L"PvE_Tertiary_Standard_Basic_1_Max" }, { L"PvE_Tertiary_Standard_Basic_1_Min", L"PvE_Tertiary_Standard_Basic_1_Max" } } },
            { L"Common", L"PvE_Tertiary_Standard_Common_AllPlayers_RespawnTime", { { L"PvE_Tertiary_Standard_Common_1_Min", L"PvE_Tertiary_Standard_Common_1_Max" }, { L"PvE_Tertiary_Standard_Common_1_Min", L"PvE_Tertiary_Standard_Common_1_Max" }, { L"PvE_Tertiary_Standard_Common_1_Min", L"PvE_Tertiary_Standard_Common_1_Max" }, { L"PvE_Tertiary_Standard_Common_1_Min", L"PvE_Tertiary_Standard_Common_1_Max" } } },
            { L"Uncommon", L"PvE_Tertiary_Standard_Uncommon_AllPlayers_RespawnTime", { { L"PvE_Tertiary_Standard_Uncommon_1_Min", L"PvE_Tertiary_Standard_Uncommon_1_Max" }, { L"PvE_Tertiary_Standard_Uncommon_1_Min", L"PvE_Tertiary_Standard_Uncommon_1_Max" }, { L"PvE_Tertiary_Standard_Uncommon_1_Min", L"PvE_Tertiary_Standard_Uncommon_1_Max" }, { L"PvE_Tertiary_Standard_Uncommon_1_Min", L"PvE_Tertiary_Standard_Uncommon_1_Max" } } },
            { L"Rare", L"PvE_Tertiary_Standard_Rare_AllPlayers_RespawnTime", { { L"PvE_Tertiary_Standard_Rare_1_Min", L"PvE_Tertiary_Standard_Rare_1_Max" }, { L"PvE_Tertiary_Standard_Rare_1_Min", L"PvE_Tertiary_Standard_Rare_1_Max" }, { L"PvE_Tertiary_Standard_Rare_1_Min", L"PvE_Tertiary_Standard_Rare_1_Max" }, { L"PvE_Tertiary_Standard_Rare_1_Min", L"PvE_Tertiary_Standard_Rare_1_Max" } } },
          } },
        { L"Encounter.Type.PvP.Wave",
          {
            { L"Basic", L"PvP_Wave_Basic_AllPlayers_RespawnTime", { { L"PvP_Wave_Basic_AllPlayers_Max", L"PvP_Wave_Basic_AllPlayers_Min" } } },
            { L"Common", L"PvP_Wave_Common_AllPlayers_RespawnTime", { { L"PvP_Wave_Common_AllPlayers_Max", L"PvP_Wave_Common_AllPlayers_Min" } } },
            { L"Uncommon", L"PvP_Wave_Uncommon_AllPlayers_RespawnTime", { { L"PvP_Wave_Uncommon_AllPlayers_Max", L"PvP_Wave_Uncommon_AllPlayers_Min" } } },
            { L"Rare", L"PvP_Wave_Rare_AllPlayers_RespawnTime", { { L"PvP_Wave_Rare_AllPlayers_Max", L"PvP_Wave_Rare_AllPlayers_Min" } } },
          } },
        { L"Encounter.Type.PvP.DevastatingWave",
          {
            { L"Basic", L"PvP_DevastatingWave_Basic_AllPlayers_RespawnTime", { { L"PvP_DevastatingWave_Basic_AllPlayers_Max", L"PvP_DevastatingWave_Basic_AllPlayers_Min" } } },
            { L"Common", L"PvP_DevastatingWave_Common_AllPlayers_RespawnTime", { { L"PvP_DevastatingWave_Common_AllPlayers_Max", L"PvP_DevastatingWave_Common_AllPlayers_Min" } } },
            { L"Uncommon", L"PvP_DevastatingWave_Uncommon_AllPlayers_RespawnTime", { { L"PvP_DevastatingWave_Uncommon_AllPlayers_Max", L"PvP_DevastatingWave_Uncommon_AllPlayers_Min" } } },
            { L"Rare", L"PvP_DevastatingWave_Rare_AllPlayers_RespawnTime", { { L"PvP_DevastatingWave_Rare_AllPlayers_Max", L"PvP_DevastatingWave_Rare_AllPlayers_Min" } } },
          } },
        { L"Encounter.Type.PvP.BluGloCamp",
          {
            { L"Basic", L"PvP_BluGloCamp_Basic_AllPlayers_RespawnTime", { { L"PvP_BluGloCamp_Basic_AllPlayers_Max", L"PvP_BluGloCamp_Basic_AllPlayers_Max" } } },
            { L"Common", L"PvP_BluGloCamp_Common_AllPlayers_RespawnTime", { { L"PvP_BluGloCamp_Common_AllPlayers_Max", L"PvP_BluGloCamp_Common_AllPlayers_Max" } } },
            { L"Uncommon", L"PvP_BluGloCamp_Uncommon_AllPlayers_RespawnTime", { { L"PvP_BluGloCamp_Uncommon_AllPlayers_Max", L"PvP_BluGloCamp_Uncommon_AllPlayers_Max" } } },
            { L"Rare", L"PvP_BluGloCamp_Rare_AllPlayers_RespawnTime", { { L"PvP_BluGloCamp_Rare_AllPlayers_Max", L"PvP_BluGloCamp_Rare_AllPlayers_Max" } } },
          } },
        { L"Encounter.Type.PvP.LootCamp",
          {
            { L"Basic", L"PvP_LootCamp_Basic_AllPlayers_RespawnTime", { { L"PvP_LootCamp_Basic_AllPlayers_Max", L"PvP_LootCamp_Basic_AllPlayers_Max" } } },
            { L"Common", L"PvP_LootCamp_Common_AllPlayers_RespawnTime", { { L"PvP_LootCamp_Common_AllPlayers_Max", L"PvP_LootCamp_Common_AllPlayers_Max" } } },
            { L"Uncommon", L"PvP_LootCamp_Uncommon_AllPlayers_RespawnTime", { { L"PvP_LootCamp_Uncommon_AllPlayers_Max", L"PvP_LootCamp_Uncommon_AllPlayers_Max" } } },
            { L"Rare", L"PvP_LootCamp_Rare_AllPlayers_RespawnTime", { { L"PvP_LootCamp_Rare_AllPlayers_Max", L"PvP_LootCamp_Rare_AllPlayers_Max" } } },
          } },
        { L"Encounter.Type.ScriptedSpawn",
          {
            { L"Basic", L"ScriptedSpawn_Basic_AllPlayers_RespawnTime", { { L"ScriptedSpawn_Basic_AllPlayers_Max", L"ScriptedSpawn_Basic_AllPlayers_Max" } } },
            { L"Common", L"ScriptedSpawn_Basic_AllPlayers_RespawnTime", { { L"ScriptedSpawn_Basic_AllPlayers_Max", L"ScriptedSpawn_Basic_AllPlayers_Max" } } },
            { L"Uncommon", L"ScriptedSpawn_Basic_AllPlayers_RespawnTime", { { L"ScriptedSpawn_Basic_AllPlayers_Max", L"ScriptedSpawn_Basic_AllPlayers_Max" } } },
            { L"Rare", L"ScriptedSpawn_Basic_AllPlayers_RespawnTime", { { L"ScriptedSpawn_Basic_AllPlayers_Max", L"ScriptedSpawn_Basic_AllPlayers_Max" } } },
          } },
    };
    const FSpawnPointsProfileDefaults SpawnPointsDefaults[] =
    {
        { L"Encounter.Type.PvE.Primary.Standard", { L"PvE_Primary_Standard_1_MaxSpawnPoints", L"PvE_Primary_Standard_2_MaxSpawnPoints", L"PvE_Primary_Standard_3_MaxSpawnPoints", L"PvE_Primary_Standard_4_MaxSpawnPoints" }, { L"PvE_Primary_Standard_1_MinSpawnPoints", L"PvE_Primary_Standard_2_MinSpawnPoints", L"PvE_Primary_Standard_3_MinSpawnPoints", L"PvE_Primary_Standard_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.NonScaling.4Player", { L"PvE_Primary_Standard_4_MaxSpawnPoints" }, { L"PvE_Primary_Standard_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT01.4Player", { L"PvE_Primary_Outpost_T01_4_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T01_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT02.4Player", { L"PvE_Primary_Outpost_T02_4_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T02_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT03.4Player", { L"PvE_Primary_Outpost_T03_4_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T03_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT04.4Player", { L"PvE_Primary_Outpost_T04_4_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T04_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.Group.4Player", { L"PvE_Primary_Group_4_MaxSpawnPoints" }, { L"PvE_Primary_Group_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.NonScaling.3Player", { L"PvE_Primary_Standard_3_MaxSpawnPoints" }, { L"PvE_Primary_Standard_3_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT01.3Player", { L"PvE_Primary_Outpost_T01_3_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T01_3_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT02.3Player", { L"PvE_Primary_Outpost_T02_3_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T02_3_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT03.3Player", { L"PvE_Primary_Outpost_T03_3_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T03_3_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT04.3Player", { L"PvE_Primary_Outpost_T04_3_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T04_3_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.NonScaling.2Player", { L"PvE_Primary_Standard_2_MaxSpawnPoints" }, { L"PvE_Primary_Standard_2_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT01.2Player", { L"PvE_Primary_Outpost_T01_2_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T01_2_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT02.2Player", { L"PvE_Primary_Outpost_T02_2_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T02_2_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT03.2Player", { L"PvE_Primary_Outpost_T03_2_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T03_2_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT04.2Player", { L"PvE_Primary_Outpost_T04_2_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T04_2_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.NonScaling.1Player", { L"PvE_Primary_Standard_1_MaxSpawnPoints" }, { L"PvE_Primary_Standard_1_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT01.1Player", { L"PvE_Primary_Outpost_T01_1_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T01_1_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT02.1Player", { L"PvE_Primary_Outpost_T02_1_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T02_1_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT03.1Player", { L"PvE_Primary_Outpost_T03_1_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T03_1_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Primary.OutpostT04.1Player", { L"PvE_Primary_Outpost_T04_1_MaxSpawnPoints" }, { L"PvE_Primary_Outpost_T04_1_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Secondary.Standard", { L"PvE_Secondary_Standard_1_MaxSpawnPoints", L"PvE_Secondary_Standard_2_MaxSpawnPoints", L"PvE_Secondary_Standard_3_MaxSpawnPoints", L"PvE_Secondary_Standard_4_MaxSpawnPoints" }, { L"PvE_Secondary_Standard_1_MinSpawnPoints", L"PvE_Secondary_Standard_2_MinSpawnPoints", L"PvE_Secondary_Standard_3_MinSpawnPoints", L"PvE_Secondary_Standard_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Secondary.NonScaling.4Player", { L"PvE_Secondary_Standard_4_MaxSpawnPoints" }, { L"PvE_Secondary_Standard_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Secondary.NonScaling.3Player", { L"PvE_Secondary_Standard_3_MaxSpawnPoints" }, { L"PvE_Secondary_Standard_3_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Secondary.NonScaling.2Player", { L"PvE_Secondary_Standard_2_MaxSpawnPoints" }, { L"PvE_Secondary_Standard_2_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Secondary.NonScaling.1Player", { L"PvE_Secondary_Standard_1_MaxSpawnPoints" }, { L"PvE_Secondary_Standard_1_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Tertiary.Standard", { L"PvE_Tertiary_Standard_1_MaxSpawnPoints", L"PvE_Tertiary_Standard_2_MaxSpawnPoints", L"PvE_Tertiary_Standard_3_MaxSpawnPoints", L"PvE_Tertiary_Standard_4_MaxSpawnPoints" }, { L"PvE_Tertiary_Standard_1_MinSpawnPoints", L"PvE_Tertiary_Standard_2_MinSpawnPoints", L"PvE_Tertiary_Standard_3_MinSpawnPoints", L"PvE_Tertiary_Standard_4_MinSpawnPoints" } },
        { L"Encounter.Type.PvP.Wave", { L"PvP_Wave_All_MaxSpawnPoints" }, { L"PvP_Wave_All_MinSpawnPoints" } },
        { L"Encounter.Type.PvP.DevastatingWave", { L"PvP_DevastatingWave_All_MaxSpawnPoints" }, { L"PvP_DevastatingWave_All_MinSpawnPoints" } },
        { L"Encounter.Type.PvP.BluGloCamp", { L"PvP_BluGloCamp_All_MaxSpawnPoints" }, { L"PvP_BluGloCamp_All_MinSpawnPoints" } },
        { L"Encounter.Type.PvP.LootCamp", { L"PvP_LootCamp_All_MaxSpawnPoints" }, { L"PvP_LootCamp_All_MinSpawnPoints" } },
        { L"Encounter.Type.PvE.Secondary.Mimic", { L"PvE_Secondary_Mimic_All_MaxSpawnPoints" }, { L"PvE_Secondary_Mimic_All_MinSpawnPoints" } },
        { L"Encounter.Type.ScriptedSpawn", { L"ScriptedSpawn_All_MaxSpawnPoints" }, { L"ScriptedSpawn_All_MaxSpawnPoints" } },
    };


    template <typename EnumType>
    EnumType EnumValue(const wchar_t* EnumeratorName)
    {
        return EnumType{ EnumType::StaticEnumInfo().FindValue(EnumeratorName) };
    }

    template <typename T>
    T* LoadDefault(const wchar_t* Path)
    {
        T* Object = LoadObject<T>(nullptr, Path);
        if (!Object)
        {
            UE_LOG(LogFortAIDirector, Warning, TEXT("AIDirector: %s didn't load"), Path);
        }
        return Object;
    }

    FCurveTableRowHandle& SetCurveTableRow(FCurveTableRowHandle& Handle, UCurveTable* CurveTable, const wchar_t* RowName)
    {
        Handle.CurveTable = CurveTable;
        Handle.RowName = FName(RowName);
        return Handle;
    }

    // Whether the curve table has the row, by UDataTableFunctionLibrary::EvaluateCurveTableRow (OutResult RowFound is 0).
    bool CurveTableHasRow(UCurveTable* CurveTable, const wchar_t* RowName)
    {
        UFunction* Evaluate = FindObject<UFunction>(nullptr, L"/Script/Engine.DataTableFunctionLibrary:EvaluateCurveTableRow");
        UClass* Library = FindObject<UClass>(nullptr, L"/Script/Engine.DataTableFunctionLibrary");
        UObject* LibraryDefault = Library ? Library->GetDefaultObject() : nullptr;
        if (!CurveTable || !Evaluate || !LibraryDefault)
        {
            return false;
        }
        Reflection::FParams Params(Evaluate, true);
        return Params.Set(L"CurveTable", reinterpret_cast<UObject*>(CurveTable)) && Params.Set(L"RowName", FName(RowName)) && Params.Set(L"InXY", 0.0f)
            && Params.Call(LibraryDefault) && Params.Take<uint8_t>(L"OutResult") == 0;
    }

    FGameplayTagContainer MakeGameplayTagContainer(std::initializer_list<const wchar_t*> TagNames)
    {
        FGameplayTagContainer Tags;
        for (const wchar_t* TagName : TagNames)
        {
            Tags.AddTag(FGameplayTag{ FName(TagName) });
        }
        return Tags;
    }

    FGameplayTagQuery MakeMatchAllTagsQuery(const wchar_t* TagName, const wchar_t* UserDescription)
    {
        FGameplayTagQuery Query = FGameplayTagQuery::MakeQuery_MatchAllTags(MakeGameplayTagContainer({ TagName }));
        Query.UserDescription = FString(UserDescription);
        Query.AutoDescription = FString((std::wstring(L" ALL( ") + TagName + L" )").c_str());
        return Query;
    }
}

AFortAIDirector* AFortAIDirector::GetCurrent(const UObject* WorldContextObject)
{
#if WITH_SERVER_CODE
    UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
    AFortGameModeZone* GameMode = World ? Cast<AFortGameModeZone>(reinterpret_cast<UObject*>(World->GetAuthGameMode())) : nullptr;
    return GameMode ? GameMode->AIDirector : nullptr;
#else
    return nullptr;
#endif
}

void AFortAIDirector::StopEncounter(UFortAIEncounterInfo* EncounterToStop, EFortObjectiveStatus EncounterStopReason, bool bForceKillAI, bool bBroadcastStop)
{
#if WITH_SERVER_CODE
    if (Natives::AFortAIDirector_StopEncounter)
    {
        Natives::AFortAIDirector_StopEncounter(this, reinterpret_cast<UObject*>(EncounterToStop), EncounterStopReason, bForceKillAI, bBroadcastStop);
    }
#endif
}

FUtilityData& AFortAIDirector::UtilityContributionDataAt(int32_t Index) const
{
    uint8_t* const First = reinterpret_cast<uint8_t*>(&GetMember_UtilityContributionData());
    return *reinterpret_cast<FUtilityData*>(First + static_cast<size_t>(Index) * FUtilityData::StaticStruct()->GetStructureSize());
}

void AFortAIDirector::ApplyAIDirectorFortniteDefaults()
{
#if WITH_SERVER_CODE
    // 1.10's paks don't ship /Game/AIDirector/AIDirector_Fortnite (DefaultGameData's AIDirectors still name it, and the
    // load fails: "Couldn't find file for package /Game/AIDirector/AIDirector_Fortnite"), so the game mode falls back to
    // the bare AFortAIDirector. That one has no BaseEncounterClass, which StartEncounter (1.10 0x2FA580) returns early
    // without (1.9.1 starts every mission encounter as BaseGateEncounter_C), no spawn location queries, spawn point
    // budgets or spawn group caps, and no alive curves: no husk spawned anywhere (#168).
    UCurveTable* const AliveCurves = LoadDefault<UCurveTable>(AIDirectorAliveCurvesPath);
    SetCurveTableRow(MaxAliveCurve, AliveCurves, L"MaxAlive");
    SetCurveTableRow(MinAliveCurve, AliveCurves, L"MinAlive");

    FIntensityData& Intensity = IntensityInfo;
    TStructArray<FIntensityContribution>& IntensityContributions = Intensity.ContributingFactors;
    IntensityContributions.Empty();
    for (const FIntensityContributionDefaults& Defaults : IntensityContributionDefaults)
    {
        FIntensityContribution& Contribution = IntensityContributions.AddDefaulted();
        Contribution.CombatFactor = EnumValue<EFortCombatFactors>(Defaults.CombatFactor);
        Contribution.ContributingAIDirectorFactor = EnumValue<EFortAIDirectorFactor>(Defaults.ContributingAIDirectorFactor);
        Contribution.MaxContribution = Defaults.MaxContribution;
        Contribution.bModifyContributionByCompletionPercentage = Defaults.bModifyContributionByCompletionPercentage;
        Contribution.bModifyByNumberOfCriticalEncounterGoals = Defaults.bModifyByNumberOfCriticalEncounterGoals;
    }
    Intensity.ContributionsTotal = 10.0f;
    TArray<UObject*>& ExceptionEditModes = Intensity.GetMember_ExceptionEditModes();
    ExceptionEditModes.Empty();
    for (const wchar_t* EditModePath : { L"/Game/Building/EditModePatterns/Wall/EMP_Wall_QuarterWallHalf.EMP_Wall_QuarterWallHalf", L"/Game/Building/EditModePatterns/Wall/EMP_Wall_QuarterWallS.EMP_Wall_QuarterWallS" })
    {
        if (UObject* EditMode = LoadDefault<UObject>(EditModePath))
        {
            ExceptionEditModes.Add(EditMode);
        }
    }

    UCurveTable* const PIDCurves = LoadDefault<UCurveTable>(AIDirPIDPath);
    FFortAIEncounterPIDControllerSettings& PIDSettings = PIDControllerSettings;
    SetCurveTableRow(PIDSettings.ProportionalGain, PIDCurves, L"Proportional");
    SetCurveTableRow(PIDSettings.IntegralGain, PIDCurves, L"Integral");
    SetCurveTableRow(PIDSettings.DerivativeGain, PIDCurves, L"Derivative");

    int32_t UtilityIndex = 0;
    for (const FUtilityDataDefaults& Defaults : UtilityDefaults)
    {
        FUtilityData& Utility = UtilityContributionDataAt(UtilityIndex++);
        TStructArray<FUtilityContribution>& Contributions = Utility.ContributingFactors;
        Contributions.Empty();
        for (const FUtilityContributionDefaults& ContributionDefaults : Defaults.ContributingFactors)
        {
            FUtilityContribution& Contribution = Contributions.AddDefaulted();
            Contribution.ContributingFactor = EnumValue<EFortCombatFactors>(ContributionDefaults.ContributingFactor);
            Contribution.ContributingAIDirectorFactor = EnumValue<EFortAIDirectorFactor>(ContributionDefaults.ContributingAIDirectorFactor);
            Contribution.MaxContribution = ContributionDefaults.MaxContribution;
            Contribution.ContributionType = EnumValue<EFortFactorContributionType>(ContributionDefaults.ContributionType);
        }
        if (Defaults.bApplyRecentSelectionPenalty)
        {
            Utility.bApplyRecentSelectionPenalty = *Defaults.bApplyRecentSelectionPenalty;
        }
        Utility.RecentlySelectedPenaltyPercentage = Defaults.RecentlySelectedPenaltyPercentage;
        Utility.PenaltyFallOffRate = Defaults.PenaltyFallOffRate;
        Utility.DebugGraphName = FString(Defaults.DebugGraphName);
        if (Defaults.DebugGraphColor)
        {
            Utility.DebugGraphColor = *Defaults.DebugGraphColor;
        }
    }

    UCurveTable* const PPECurves = LoadDefault<UCurveTable>(AIDirPPEPath);
    FFortPlayerPerformanceEstimateSettings& PerformanceSettings = PlayerPerformanceEstimateSettings;
    SetCurveTableRow(PerformanceSettings.PlayerPerformanceEstimateTransformMin, PPECurves, L"TransformMin");
    SetCurveTableRow(PerformanceSettings.PlayerPerformanceEstimateTransformOrigin, PPECurves, L"TransformOrigin");
    SetCurveTableRow(PerformanceSettings.PlayerPerformanceEstimateTransformMax, PPECurves, L"TransformMax");

    BurstSpawnThreatVisualsEndDelay = 5.0f;

    UCurveTable* const PopulationCategoryCaps = LoadDefault<UCurveTable>(AIDirectorPopulationCategoryCapsPath);
    TStructArray<FFortAIEncounterSpawnGroupCapsProfile>& SpawnGroupCapSettings = EncounterSpawnGroupCapSettings;
    SpawnGroupCapSettings.Empty();
    for (const FSpawnGroupCapsProfileDefaults& ProfileDefaults : SpawnGroupCapsDefaults)
    {
        FFortAIEncounterSpawnGroupCapsProfile& Profile = SpawnGroupCapSettings.AddDefaulted();
        Profile.EncounterTypeTags = MakeGameplayTagContainer({ ProfileDefaults.EncounterTypeTag });
        TStructArray<FFortAIEncounterSpawnGroupCapsCategory>& Categories = Profile.PopulationCategories;
        for (const FSpawnGroupCapsCategoryDefaults& CategoryDefaults : ProfileDefaults.PopulationCategories)
        {
            FFortAIEncounterSpawnGroupCapsCategory& Category = Categories.AddDefaulted();
            const std::wstring CategoryTag = std::wstring(L"Encounter.PopulationCategory.") + CategoryDefaults.Category;
            const std::wstring Description = std::wstring(CategoryDefaults.Category) + L" Population Category";
            Category.TagQuery = MakeMatchAllTagsQuery(CategoryTag.c_str(), Description.c_str());
            Category.bApplyGroupPopulationCurveToCategoryMax = true;
            SetCurveTableRow(Category.SpawnGroupAvailabilityDelaySeconds, PopulationCategoryCaps, CategoryDefaults.SpawnGroupAvailabilityDelaySecondsRow);
            TStructArray<FFortAIEncounterSpawnGroupCap>& Caps = Category.SpawnGroupCapsPerPlayerCount;
            for (const FSpawnGroupCapDefaults& CapDefaults : CategoryDefaults.SpawnGroupCapsPerPlayerCount)
            {
                FFortAIEncounterSpawnGroupCap& Cap = Caps.AddDefaulted();
                SetCurveTableRow(Cap.MinSpawnGroupNumberCap, PopulationCategoryCaps, CapDefaults.MinSpawnGroupNumberCapRow);
                SetCurveTableRow(Cap.MaxSpawnGroupNumberCap, PopulationCategoryCaps, CapDefaults.MaxSpawnGroupNumberCapRow);
            }
            Category.InitialSpawnGroupAvailabilityTime = -1.0f;
            Category.NumActiveCategorySpawnGroups = 0.0f;
            Category.NumSpawnGroupAvailable = 0;
        }
    }

    UCurveTable* const SpawnBudgetCurves = LoadDefault<UCurveTable>(AIDirectorSpawnBudgetCurvesPath);
    TStructArray<FFortAIEncounterSpawnPointsProfile>& SpawnPointsSettings = EncounterSpawnPointsSettings;
    SpawnPointsSettings.Empty();
    for (const FSpawnPointsProfileDefaults& ProfileDefaults : SpawnPointsDefaults)
    {
        FFortAIEncounterSpawnPointsProfile& Profile = SpawnPointsSettings.AddDefaulted();
        Profile.EncounterTypeTags = MakeGameplayTagContainer({ ProfileDefaults.EncounterTypeTag });
        TStructArray<FCurveTableRowHandle>& MaxSpawnPoints = Profile.MaxSpawnPointsPerPlayerCount;
        for (const wchar_t* RowName : ProfileDefaults.MaxSpawnPointsPerPlayerCountRows)
        {
            SetCurveTableRow(MaxSpawnPoints.AddDefaulted(), SpawnBudgetCurves, RowName);
        }
        TStructArray<FCurveTableRowHandle>& MinSpawnPoints = Profile.MinSpawnPointsPerPlayerCount;
        for (const wchar_t* RowName : ProfileDefaults.MinSpawnPointsPerPlayerCountRows)
        {
            SetCurveTableRow(MinSpawnPoints.AddDefaulted(), SpawnBudgetCurves, RowName);
        }
    }

    TStructArray<FFortAIEncounterPawnDifficultyLevelModifier>& DifficultyLevelModifiers = EncounterPawnDifficultyLevelModifiers;
    DifficultyLevelModifiers.Empty();
    UCurveTable* const GroupEnemyOffsets = LoadDefault<UCurveTable>(GroupEnemyOffsetsPath);
    FFortAIEncounterPawnDifficultyLevelModifier& FourPlayerModifier = DifficultyLevelModifiers.AddDefaulted();
    FourPlayerModifier.EncounterTagRequirementsQuery = MakeMatchAllTagsQuery(L"Encounter.EnemyDifficulty.4Player", L"");
    SetCurveTableRow(FourPlayerModifier.DifficultyLevelModifierCurve, GroupEnemyOffsets, L"EnemyOffset_4Player");
    // 🟡 Inferred: the BluGlo Difficulty Pylon (2.5+, CoreV2 #225). Its first two boosts' mutators (DifficultyBoost01/02's
    // Boost01/02AllEncountersMutator, AFortGameplayMutator_AIEncounterModifierTags) tag every encounter
    // Encounter.EnemyDifficulty.2Player / .3Player, and 3.0's GroupEnemyOffsets has EnemyOffset_2Player / _3Player for
    // them, but 1.9.1's AIDirector_Fortnite (all these settings) only had the 4Player modifier and the later builds' didn't
    // ship. Without these the first two boosts left the enemies' level as it was. Only where the rows exist.
    for (const auto& [TagName, RowName] : { std::pair{ L"Encounter.EnemyDifficulty.3Player", L"EnemyOffset_3Player" }, std::pair{ L"Encounter.EnemyDifficulty.2Player", L"EnemyOffset_2Player" } })
    {
        if (!CurveTableHasRow(GroupEnemyOffsets, RowName))
        {
            continue;
        }
        FFortAIEncounterPawnDifficultyLevelModifier& Modifier = DifficultyLevelModifiers.AddDefaulted();
        Modifier.EncounterTagRequirementsQuery = MakeMatchAllTagsQuery(TagName, L"");
        SetCurveTableRow(Modifier.DifficultyLevelModifierCurve, GroupEnemyOffsets, RowName);
    }

    // 1.11's Mini-Bosses (#191), which 1.9.1's director predates. A Mini-Boss mission alert's modifier,
    // MiniBossEnablePrimaryMissionItem, spawns MiniBossEnablePrimaryMissionMutator (an
    // AFortGameplayMutator_AIEncounterModifierTags), which tags the mission's Primary encounters Encounter.Enable.MiniBoss.
    // The director (2.2 0x2C90F0, called by the encounter's tag refresh at 0x2CBC02) offers an encounter the
    // SpawnGroupUpgrades whose EncounterTagRequirementsQuery its tags match, and only there does the Mini-Boss upgrade
    // come from. The option sets of the Mini-Boss missions (EDO_Set_RetrieveData, _Gate, _LaunchRocket3, _LaunchBalloon2,
    // _DeliverBomb3, _Survival_1) unlock its population category after 1, 3 or 5 minutes
    // (OptionalMiniBossStartTimedTagsCategory: Encounter.PopulationCategory.Enable.MiniBoss). The paks ship one complete
    // upgrade, the 1P one (SGU_MiniBoss_1P, SUP_MiniBoss_1P, SGC_MiniBoss_1P; SGU_MiniBoss_4P has no probabilities or
    // caps of its own), so that is the one offered: AIDirector_Fortnite's own list didn't ship, so the choice is Core's.
    TStructArray<FFortAISpawnGroupUpgradeData>& Upgrades = SpawnGroupUpgrades;
    Upgrades.Empty();
    UClass* const MiniBossUpgrade = LoadObject<UClass>(nullptr, MiniBossSpawnGroupUpgradePath);
    if (MiniBossUpgrade)
    {
        FFortAISpawnGroupUpgradeData& MiniBoss = Upgrades.AddDefaulted();
        MiniBoss.SpawnGroupUpgrade = MiniBossUpgrade;
        MiniBoss.UpgradeProbabilities = LoadDefault<UClass>(L"/Game/AIDirector/MiniBoss/SpawnUpgradeProbabilities/SUP_MiniBoss_1P.SUP_MiniBoss_1P_C");
        MiniBoss.SpawnGroupCapsCategories = LoadDefault<UClass>(L"/Game/AIDirector/MiniBoss/SpawnGroupCaps/SGC_MiniBoss_1P.SGC_MiniBoss_1P_C");
        MiniBoss.EncounterTagRequirementsQuery = MakeMatchAllTagsQuery(L"Encounter.Enable.MiniBoss", L"");
    }

    float* const DesireMappings = &GetMember_DiscreteEncounterUtilityDesireMappings();
    DesireMappings[0] = 2.5f;
    DesireMappings[1] = 5.0f;
    DesireMappings[2] = 7.5f;
    DesireMappings[3] = 10.0f;

    // Element 2 keeps AFortAIDirector's.
    EFortAIUtility* const DynamicUtilities = &GetMember_InitialDynamicUtilities();
    DynamicUtilities[0] = EnumValue<EFortAIUtility>(L"DestroyBuildingsMelee");
    DynamicUtilities[1] = EnumValue<EFortAIUtility>(L"KillPlayersRanged");
    DynamicUtilities[3] = EnumValue<EFortAIUtility>(L"Mob");

    // Elements 0 and 1; 2 and 3 keep AFortAIDirector's.
    const int32_t QueryInfoSize = FEncounterEnvironmentQueryInfo::StaticStruct()->GetStructureSize();
    uint8_t* const PlacementQueries = reinterpret_cast<uint8_t*>(&GetMember_SpawnLocationPlacementQueries());
    FEncounterEnvironmentQueryInfo& RandomDirectionQuery = *reinterpret_cast<FEncounterEnvironmentQueryInfo*>(PlacementQueries);
    RandomDirectionQuery.EnvironmentQuery = LoadDefault<UEnvQuery>(L"/Game/AIDirector/EQS/EncounterRandomDirectionQuery.EncounterRandomDirectionQuery");
    TStructArray<FEnvNamedValue>& RandomDirectionParams = RandomDirectionQuery.QueryParams;
    RandomDirectionParams.Empty();
    FEnvNamedValue& AngleParam = RandomDirectionParams.AddDefaulted();
    AngleParam.ParamName = FName(L"Angle");
    AngleParam.ParamType = EnumValue<EAIParamType>(L"Float");
    AngleParam.Value = 0.95f;
    RandomDirectionQuery.bIsDirectional = true;
    FEncounterEnvironmentQueryInfo& MinDistanceQuery = *reinterpret_cast<FEncounterEnvironmentQueryInfo*>(PlacementQueries + QueryInfoSize);
    MinDistanceQuery.EnvironmentQuery = LoadDefault<UEnvQuery>(L"/Game/AIDirector/EQS/EncounterMinDistanceNavGraph.EncounterMinDistanceNavGraph");

    DataTrackingSettings = LoadDefault<UObject>(L"/Game/AIDirector/AIDirectorFactors/DataTracking_Default.DataTracking_Default");
    FFortEncounterPawnNumberCaps& PawnCaps = DefaultEncounterPawnCaps;
    PawnCaps.bApplyPawnNumberCaps = false;
    EncounterPawnSpawnInterval = 1.0f;
    DummyDebugEncounter = LoadDefault<UClass>(L"/Game/AIDirector/Encounters/DummyDebugEncounter.DummyDebugEncounter_C");
    BaseEncounterClass = LoadDefault<UClass>(L"/Game/AIDirector/Encounters/BaseGateEncounter.BaseGateEncounter_C");
    MaxActiveAlive = 60;
    NumWorldSubdivides = 7;
    MinAISpawnDistanceFromPlayers = 4000.0f;
    AmbientThreatPreferredPlacementActorTags = MakeGameplayTagContainer({ L"Activities.Placement.Hidden", L"Activities.Placement.Indoor", L"Activities.Placement.LocationBasement",
        L"Activities.Placement.LocationCommercial", L"Activities.Placement.LocationCave", L"Activities.Placement.LocationIndustrial", L"Activities.Placement.LocationResidential",
        L"Activities.Placement.LocationRoof", L"Activities.Placement.OnDynamic" });
    AmbientThreatRequiredPlacementActorTags = MakeGameplayTagContainer({ L"PlacementActor" });
    MaxNormalLODDistanceToPlayer = 1000.0f;
    AIRelevantDistanceToPlayer = 2000.0f;
    EncounterRelevantDistanceToPlayer = 6000.0f;

    const TSubclassOf<UFortAIEncounterInfo> DebugEncounter = DummyDebugEncounter;
    const TSubclassOf<UFortAIEncounterInfo> BaseEncounter = BaseEncounterClass;
    const int32_t NumEncounterClasses = (DebugEncounter.Get() ? 1 : 0) + (BaseEncounter.Get() ? 1 : 0);
    const int32_t NumSpawnQueries = (RandomDirectionQuery.EnvironmentQuery ? 1 : 0) + (MinDistanceQuery.EnvironmentQuery ? 1 : 0);
    UE_LOG(LogFortAIDirector, Log, TEXT("AIDirector: this build has no AIDirector_Fortnite (a server assets pak puts it back), the native director takes its 1.9.1 settings (%d encounter classes, %d spawn queries, %d spawn group cap profiles, %d spawn point profiles, %d spawn group upgrades)"),
        NumEncounterClasses, NumSpawnQueries, SpawnGroupCapSettings.Num(), SpawnPointsSettings.Num(), Upgrades.Num());
#endif
}
