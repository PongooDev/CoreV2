#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

class UClass;
class UFunction;
class UObject;

namespace Finder
{
    struct FEarnScoreCallSites
    {
        uint8_t* GateCall = nullptr;
        uint8_t* ScoreCall = nullptr;
        uint8_t* GrowCall = nullptr;
        uint8_t* ContextCall = nullptr;
    };

    struct FConditionalFoundationQuotaSites
    {
        uintptr_t Quota = 0;
        uint8_t* QuotaCall = nullptr;
        ptrdiff_t TierIndexOffset = -1;
        uint8_t* ContextCall = nullptr;
        uint8_t* SelectCall = nullptr;
        int32_t RequestSize = 0;
        ptrdiff_t RequestClassesOffset = -1;
        ptrdiff_t RequestsArrayOffset = -1;
    };

    struct FWorldLootSites
    {
        uintptr_t Quota = 0;
        uint8_t* ZoneQuotaCall = nullptr;
        uint8_t* TileQuotaCall = nullptr;
        uint8_t* TileRecordCall = nullptr;
        bool bSpawnsTileLoot = false;
    };

    struct FSpawnGroupUpgradeSites
    {
        uintptr_t TryUpgrade = 0;
        uint8_t* TryUpgradeCall = nullptr;
        uint8_t* GroupCostCall = nullptr;
        uint8_t* PawnUpgradeCall = nullptr;
        bool bRollsModifiers = false;
    };

    struct FPlayerSpawnQuerySites
    {
        uintptr_t PickSpawnActor = 0;
        uint8_t* FriendlyCalls[2] = {};
        uint8_t* InitialCall = nullptr;
        uint8_t* BackupCall = nullptr;
        int32_t FriendlyPawnsIndex = -1;
    };

    uintptr_t ImageBase();
    uintptr_t TextBegin();
    uintptr_t TextEnd();

    std::vector<uintptr_t> StringRefs(const wchar_t* Text);
    std::vector<uintptr_t> StringRefs(const char* Text);
    // The first copy of the bytes in .rdata, or 0.
    uintptr_t FindReadOnly(const void* Bytes, size_t Size);

    uintptr_t FunctionStart(uintptr_t Address);
    uintptr_t FunctionEnd(uintptr_t FunctionStart);

    uintptr_t RelativeTarget(uintptr_t Instruction, uint32_t OperandOffset, uint32_t InstructionLength);
    uintptr_t NextCall(uintptr_t From, size_t MaxDistance);
    std::vector<std::pair<uintptr_t, uintptr_t>> DirectCalls(uintptr_t Begin, uintptr_t End);
    std::vector<uintptr_t> CallsTo(uintptr_t Target, bool bIncludeJumps);

    int32_t VTableIndex(void** VTable, void* Function, int32_t MaxSlots);
    void** VTableContaining(uintptr_t Function, int32_t Slot, int32_t Slots);
    uintptr_t ImportSlot(const char* Module, const char* Symbol);
    bool IsReadable(const void* Pointer, size_t Size);
    uintptr_t LogCategory(const wchar_t* CategoryName);

    bool ResolveRuntime();
    bool ResolveServer();
    bool ResolveObjectGetWorld();
    bool ResolveClient();
    bool ResolveConsoleManager();
    bool ResolvePropertyCPPType();
    bool ResolveScriptStructCopy();
    bool ResolveScriptFrame();
    bool ResolveTextLocalizationManager();
    bool ResolvePlayerControllerConsole();
    bool ResolvePlayerControllerCheats();
    bool ResolveVoiceConfig();
    bool ResolvePushToTalk();
    bool ResolveOnlineVoice();
    bool ResolveVoiceHandshake();
    bool ResolvePlayerControllerConsoleCommand(uintptr_t ServerCheatImplementation);
    bool ResolveAssetManagerLive();
    bool ResolveProcessEvent();
    bool ResolveInitListen();
    bool ResolveReplicationLive();
    bool ResolveGameModeZoneLive();
    bool ResolveAbilitySystemLive();
    int32_t ImplementationIndex(UFunction* Function);
    bool ResolveWorldItemDefinitionLive();
    bool ResolvePickupLive();
    bool ResolvePickupGiveLive();
    bool ResolvePlayerDropPickupLive();
    bool ResolveMissionManagerLive(ptrdiff_t GameStateMissionManagerOffset);
    bool ResolveAIDirectorLive();
    bool ResolveLoginLive();
    bool ResolveMissionGenerationLive();
    bool ResolveMissionObjectivesLive();
    bool ResolveMissionEventLive();
    bool ResolveMissionParTimeLive(uintptr_t CustomHandleMissionEvent);
    bool ResolveMissionBadgeLive(ptrdiff_t EarnedBadgesArrayOffset);
    bool ResolveTheaterMapDataLive();
    bool ResolveHomebaseGadgetsLive();
    bool ResolveObjectiveActivationLive(ptrdiff_t MissionActiveObjectivesOffset);
    bool ResolveCraftingLive();
    uint8_t* NullGetCurrentCall(UFunction* Function);
    bool ResolveWorldItemLive(void** InventoryOwnerVTable);
    bool ResolveMcpProfileWorldLive(void** ProfileVTable);
    bool ResolveGoalManagerLive();
    bool ResolveGoalDiscouragementLive();
    bool ResolveQueryGeneratorsLive(void** EnemiesVTable);
    bool ResolveQueryContextsLive(void** EncounterPrimaryAssignmentGoalsVTable, UClass* GoalItemType);
    bool ResolveQuickBarFocusLive();
    bool ResolveSpottingLive(UClass* SpottableActorInterface);
    bool ResolveNavigationSystemLive();
    bool ResolveOutpostOwnerLive();
    bool ResolveBadgeScoresLive();
    bool ResolveEarnScoreLive(ptrdiff_t CriticalMatchBonusOffset, FEarnScoreCallSites& OutSites);
    bool ResolveLeaveOutpostLive();
    bool ResolveOutpostMcpRequestLive();
    bool ResolveResetGameplayAttributesLive();
    bool ResolveLootTierRedirectLive();
    bool ResolveQuestStatEvents();
    bool ResolveAthenaHeroLive();
    uint8_t* AircraftPhasePlayerCleanupCall();
    bool ResolveAthenaMenuHero();
    bool ResolveAthenaHUDPawnBinding();
    bool ResolveMcpProfileQuery();
    bool ResolveDBNOReviveLive(ptrdiff_t IsDBNOOffset, ptrdiff_t QuickBarsOffset);
    uint8_t* PawnDiedInventoryDropCall();
    bool ResolveDecoSpawnedLive();
    bool ResolveScoreBuildingConstructionLive();
    bool ResolveBuildingRestorationLive(ptrdiff_t EditingPlayerOffset);
    bool ResolveServerMoveLive();
    uint8_t* SharedEmptyCallInImplementation(UFunction* Function);
    bool ResolveInventoryLive();
    bool ResolveBackpackLive(ptrdiff_t PrimaryPickupItemEntryOffset);
    bool ResolveWorldItemCountLive(ptrdiff_t RegenCooldownOffset, ptrdiff_t ItemEntryOffset);
    bool ResolveItemEntryStateLive();
    bool ResolveNeverPersistItemDefinitionLive();
    bool ResolveDisassemblyLive(UFunction* ServerDisassembleInventoryItem, UObject* PlayerControllerDefault);
    std::vector<uint8_t*> GoalManagerGetCurrentCalls(void** GoalManagerVTable, void** DirectorVTable, int32_t GoalManagerSize, int32_t DirectorSize, int32_t ActorSize, int32_t GetWorldIndex, uintptr_t& OutStub, std::vector<uint8_t*>& OutDirectorCalls);
    uint8_t* FirstCallInImplementation(UFunction* Function, uintptr_t Target);
    uint8_t* ImplementationCall(UFunction* Function);
    bool ResolveZoneEndLive(ptrdiff_t MissionInfoOffset, ptrdiff_t ZoneFinishedOffset, ptrdiff_t ZoneCompletedOffset, ptrdiff_t EndOfZoneRemainTimeOffset, ptrdiff_t ActiveSpawnPadOffset);
    bool ResolveInventoryOwnerLive(UFunction* ServerRemoveInventoryItem, UObject* PlayerControllerDefault, ptrdiff_t WorldInventoryOffset);
    uint8_t* QuickBarChangedWorldManagerCall();
    uint8_t* ExplorationActorFinderCall(ptrdiff_t DistanceRangeMinOffset, ptrdiff_t DistanceRangeMaxOffset);
    uint8_t* AbilityEndCleanupCall(ptrdiff_t AbilityGrantedWeaponItemOffset);
    bool ResolveConditionalFoundationQuotaLive(ptrdiff_t QuotaOffset, FConditionalFoundationQuotaSites& OutSites);
    bool ResolvePlayerSpawnQueriesLive(FPlayerSpawnQuerySites& OutSites);
    bool ResolveAbilityWeaponLive(ptrdiff_t CurrentWeaponListOffset, ptrdiff_t LastEquippedWeaponGuidOffset);
    bool ResolveWeaponAmmoLive(ptrdiff_t AmmoCountOffset, ptrdiff_t ItemEntryGuidOffset);
    bool IsStubFunction(uintptr_t Function);
    bool ResolveLootLive();
    bool ResolveBuildingLootLive();
    bool ResolveHarvestLive(ptrdiff_t AllowResourceDropOffset, uint8_t AllowResourceDropMask);
    bool ResolveClearAnimatingMIDsLive(ptrdiff_t ClearMIDOffset, uint8_t ClearMIDMask, ptrdiff_t AnimatingMIDsOffset);
    bool ResolveLootQuotaLive(ptrdiff_t MaximumQuotaLevelBoundOffset);
    bool ResolveWorldLootLive(ptrdiff_t DefaultLootQuotaCategoryOffset, ptrdiff_t LootDataOffset, ptrdiff_t ConstructTileWorldOffset, uint8_t ConstructTileWorldMask, FWorldLootSites& OutSites);
    bool ResolveSpawnGroupUpgradeLive(ptrdiff_t AvailableUpgradesOffset, ptrdiff_t ActiveSpawnGroupsOffset, ptrdiff_t TotalGroupCostOffset, ptrdiff_t PawnUpgradesOffset, ptrdiff_t ModifiersForAllPawnsOffset, FSpawnGroupUpgradeSites& OutSites);
    bool ResolveDurabilityLive(UFunction* ServerModDurability, UObject* PlayerControllerDefault, ptrdiff_t EntryDurabilityOffset, ptrdiff_t PendingDestroyDueToDurabilityOffset, uint8_t PendingDestroyDueToDurabilityMask);
}
