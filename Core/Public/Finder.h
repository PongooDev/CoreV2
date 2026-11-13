#pragma once

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string_view>
#include <utility>
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

    // Whether the exe has more than one executable section, as 1.9.1's does: its moved functions start with jumps, and
    // their bodies replace ret with lea rsp, [rsp+8]; jmp qword ptr [rsp-8]. False for 1.1 to 1.9.
    bool HasMovedCode();
    // The exe's executable sections as [begin, end), .text first.
    const std::vector<std::pair<uintptr_t, uintptr_t>>& ExecutableSections();
    // Where the jumps at Address lead (jmp, and moved code's push/lea/xchg/ret) on an exe with moved code; Address elsewhere.
    uintptr_t Unthunk(uintptr_t Address);
    // Whether Function's code is Bytes (ending in ret or ret 0), including moved code's rewritten copy of it.
    bool BodyIs(uintptr_t Function, std::initializer_list<uint8_t> Bytes);
    // The code pointer .rdata holds most often: the shared empty function every stripped void virtual points at.
    uintptr_t MostReferencedFunction();
    // The first of up to MaxInstructions instructions run from From that Match accepts, following jumps (Unthunk), so it
    // reads moved code (split into pieces joined by jumps) in the order it runs. 0 at a return.
    uintptr_t TraceFind(uintptr_t From, size_t MaxInstructions, const std::function<bool(uintptr_t Instruction)>& Match);
    // The fewest instructions from From to Goal over every path the code can take (both sides of each branch, and
    // moved code's branches through the stack), looking at up to MaxInstructions of them; -1 if Goal isn't reached.
    int32_t RunsInto(uintptr_t From, uintptr_t Goal, size_t MaxInstructions);

    uintptr_t ImageBase();
    uintptr_t TextBegin();
    uintptr_t TextEnd();

    std::vector<uintptr_t> StringRefs(const wchar_t* Text);
    std::vector<uintptr_t> StringRefs(const char* Text);
    // Reads the code's string references (the table StringRefs looks in) again. A packed exe's stub is still decrypting
    // parts of the code on its worker threads when its entry wrapper hands over, where the table is first read, so a
    // packed server calls this once the engine is up.
    void RescanStringRefs();
    // Every lea r64, [rip+X] in executable code whose X is Target: the sites that take a function's address, which the
    // ones bound into a delegate have instead of a call.
    std::vector<uintptr_t> AddressRefs(uintptr_t Target);
    // The first copy of the bytes in .rdata, or 0.
    uintptr_t FindReadOnly(const void* Bytes, size_t Size);
    // The first copy of the bytes in .rdata past After, or 0.
    uintptr_t FindReadOnlyAfter(const void* Bytes, size_t Size, uintptr_t After);

    uintptr_t FunctionStart(uintptr_t Address);
    uintptr_t FunctionEnd(uintptr_t FunctionStart);
    // The [begin, end) ranges of the function's separated pieces: the .pdata entries whose unwind info chains back to
    // the one starting at FunctionStart (UNW_FLAG_CHAININFO). FunctionEnd only covers the first piece.
    std::vector<std::pair<uintptr_t, uintptr_t>> FunctionChunks(uintptr_t FunctionStart);

    uintptr_t RelativeTarget(uintptr_t Instruction, uint32_t OperandOffset, uint32_t InstructionLength);
    // Every `lea rcx, [reg+disp32]` (not off rsp or rbp) right before a `call rel32` in the function's pieces, in code
    // order: the displacement and the call's target. A member passed as `this` (a container in its owner, a delegate).
    std::vector<std::pair<ptrdiff_t, uintptr_t>> MemberThisCalls(uintptr_t Function);
    // The offset of the native multicast delegate the function broadcasts last, from the object it loads it off (an
    // unreflected TMulticastDelegate member): an inlined TMulticastDelegate::Broadcast (UE 4.21: lea r14, [r15+210h];
    // inc dword ptr [r14+14h], the invocation list's lock count) wins over a call of the out-of-line one (UE 4.16-4.20:
    // lea rcx, [rdx+2D0h]; call). -1 when neither is there.
    ptrdiff_t LastBroadcastDelegateOffset(uintptr_t Function);
    uintptr_t NextCall(uintptr_t From, size_t MaxDistance);
    std::vector<std::pair<uintptr_t, uintptr_t>> DirectCalls(uintptr_t Begin, uintptr_t End);
    // Every `call rel32` of the function's pieces, decoded instruction by instruction (a byte scan also takes an E8 inside
    // another instruction's displacement for one): the call's address and its target.
    std::vector<std::pair<uintptr_t, uintptr_t>> DecodedDirectCalls(uintptr_t Function);
    // Whether an instruction of the function's pieces addresses memory at [reg+Displacement] (disp32, not off rbp): a
    // member of an object it reads or writes.
    bool ReadsMemberAt(uintptr_t Function, int32_t Displacement);
    std::vector<uintptr_t> CallsTo(uintptr_t Target, bool bIncludeJumps);

    int32_t VTableIndex(void** VTable, void* Function, int32_t MaxSlots);
    void** VTableContaining(uintptr_t Function, int32_t Slot, int32_t Slots);
    // Every .rdata slot that holds Function: its entries in vtables.
    std::vector<void**> ReadOnlySlotsHolding(uintptr_t Function);
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
    // UClass::FuncMap's offset (ObjectLayout UClass_FuncMap), which UClass::AddNativeFunction adds Core's natives to.
    // Game thread, once the engine's reflection is up; false (logged once) where it isn't found.
    bool ResolveClassFunctionMap();
    bool ResolveTextLocalizationManager();
    bool ResolvePlayerControllerConsole();
    bool ResolvePlayerControllerCheats();
    bool ResolveVoiceConfig();
    // GConfig, GEngineIni, FConfigCacheIni::Find and FConfigFile::CombineFromBuffer (the online hotfix's ini merge).
    bool ResolveConfigCombine();
    // FOnlineSubsystemImpl::IsDedicated and the number of candidates (one when found); ResolveServer requires it.
    uintptr_t FindOnlineSubsystemIsDedicated(int32_t& OutMatches);
    // The same, setting Natives::FOnlineSubsystemImpl_IsDedicated and bForceDedicated, for use before ResolveServer.
    bool ResolveOnlineSubsystemIsDedicated();
    bool ResolvePushToTalk();
    bool ResolveOnlineVoice();
    // UEngine::Init's vtable slot, from FEngineLoop::Init's GEngine->Init call (needs GEngine).
    bool ResolveEngineInit();
    // FBaseServiceMcp::GetBasicAuth, which every MCP service's vtable holds (packed builds, FFortMcpConfig).
    bool ResolveMcpBasicAuth();
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
    // From 6.20 (Battle Royale's AI, the Fortnitemares Cube Monsters): the direct call AFortGameModeAthena's own
    // CreateAIDirector makes to AFortGameModeZone::CreateAIDirector (Super::), stripped into the shared empty function.
    // AISettingsOffset: AFortGameModeAthena's AISettings, which the function tests first. Null where the build's Athena
    // game mode has no CreateAIDirector of its own (the zone's slot, already swapped) or none of that shape.
    uint8_t* AthenaCreateAIDirectorSuperCall(ptrdiff_t AISettingsOffset);
    // From 6.x (AI pawns that animate through the animation sharing system: 6.20's Fortnitemares Cube Monsters): what
    // the build keeps of UFortAbilitySystemComponent::PlaySharedAnim (a call of UFortAbilityTask_PlayAnimWaitTarget's
    // overrides whose target only writes RepSharedAnimInfoOffset), every call of it, and AFortPawn::
    // TriggerAbilityFromAnimation's slot: Natives::UFortAbilitySystemComponent_PlaySharedAnim(Calls),
    // AFortPawn_TriggerAbilityFromAnimationIndex. False (quietly) on builds without the task.
    bool ResolveSharedAnimLive(ptrdiff_t RepSharedAnimInfoOffset);
    // From 6.x: UAthenaAIPerceptionManager::OnAISettingsUpdated, the call UAthenaAISystem::StartPlay makes with the game
    // mode's AISettings (Natives::UAthenaAIPerceptionManager_OnAISettingsUpdated). False (quietly) without UAthenaAISystem.
    bool ResolveAthenaAIPerceptionLive();
    bool ResolveLoginLive();
    bool ResolveMissionGenerationLive();
    bool ResolveMissionObjectivesLive();
    bool ResolveMissionEventLive();
    bool ResolveMissionEventParamsLive();
    bool ResolveMissionParTimeLive(uintptr_t CustomHandleMissionEvent);
    bool ResolveMissionBadgeLive(ptrdiff_t EarnedBadgesArrayOffset);
    bool ResolveTheaterMapDataLive();
    bool ResolveHomebaseGadgetsLive();
    bool ResolveObjectiveActivationLive(ptrdiff_t MissionActiveObjectivesOffset);
    bool ResolveCraftingLive();
    uint8_t* NullGetCurrentCall(UFunction* Function);
    bool ResolveWorldItemLive(void** InventoryOwnerVTable);
    bool ResolveMcpProfileWorldLive(void** ProfileVTable);
    // AFortGameSession::OnVerifyAuthComplete's slot in SessionVTable (AFortGameSession's), and whether it tests
    // bWasSuccessful before the kick.
    bool ResolveGameSessionVerifyAuthLive(void** SessionVTable);
    bool ResolveGoalManagerLive();
    bool ResolveGoalDiscouragementLive();
    // 5.0+: the build's own AFortAIGoalManager::SetFrustrationDiscouragement, from UFortBTTask_SetFrustrationDiscouragement's
    // ExecuteTask (TaskVTable) passing DiscouragementDuration (at DurationOffset) third. False where ExecuteTask is stripped.
    bool ResolveFrustrationDiscouragementLive(void** TaskVTable, ptrdiff_t DurationOffset);
    bool ResolveQueryGeneratorsLive(void** EnemiesVTable);
    // 4.0: AFortAIPawn's death handler's slot in PawnVTable (AFortAIPawn's), from the bUsesDBNO test in it.
    bool ResolveAIPawnDeathLive(void** PawnVTable, ptrdiff_t UsesDBNOOffset, uint8_t UsesDBNOMask);
    bool ResolveAssignmentGoalChecksLive(ptrdiff_t MaxAIForAssignmentOffset, ptrdiff_t MaxAIPerGoalOffset);
    bool ResolveQueryContextsLive(void** EncounterPrimaryAssignmentGoalsVTable, UClass* GoalItemType);
    // Whether Function (a vtable entry of a goal manager user) has lost its AFortAIGoalManager::GetCurrent: its body can
    // be read and calls neither GetCurrent's stub nor a call site AFortAIGoalManager::HookCallSites redirected. 6.21
    // CL-4526925's compiler (and 3.2's) folds such a body down to the casts before the call.
    bool IsFoldedGoalManagerUser(uintptr_t Function);
    // AFortAIGoalManager::RegisterAIForGoalDetection, from AFortAIController's Possess (AIControllerVTable is its own),
    // and UnregisterAIForGoalDetection, from AFortAIController::UnregisterForGoalDetection.
    bool ResolveGoalDetectionLive(void** AIControllerVTable);
    // AFortAIGoalManager::OnReceiveGoalQueryResult's query result handling and FAIAssignmentInfo::LastQueryResult, from
    // AFortAIController::OnReceiveGoalQueryResult; SuppressGoalUpdatesOffset checks its bSuppressGoalUpdates test.
    bool ResolveGoalQueryResultLive(ptrdiff_t SuppressGoalUpdatesOffset);
    // UFortBTService_ContextOverride::ApplyValueOverrides / RevertValueOverrides slots, from its OnBecomeRelevant and
    // OnCeaseRelevant (ContextOverrideVTable is its own).
    bool ResolveContextOverrideLive(void** ContextOverrideVTable);
    bool ResolveQuickBarFocusLive();
    bool ResolveSpottingLive(UClass* SpottableActorInterface);
    bool ResolveNavigationSystemLive();
    bool ResolveNavigationSystemInitializeForWorldLive(ptrdiff_t WorldNavigationSystemOffset);
    // 4.2: FNavigationSystem::AddNavigationSystemToWorld (Natives), from the world's NavigationSystem and the world
    // settings' NavigationSystemConfig offsets.
    bool ResolveAddNavigationSystemToWorldLive(ptrdiff_t WorldNavigationSystemOffset, ptrdiff_t NavigationSystemConfigOffset);
    // dtNavMesh::addTile and the native recast members around it (Natives::dtNavMesh_addTile and the offsets after it),
    // from FRecastNavMeshGenerator::AddGeneratedTiles, for Battle Royale's navmesh tile cache.
    bool ResolveRecastNavMeshLive();
    bool ResolveOutpostOwnerLive();
    bool ResolveBadgeScoresLive();
    bool ResolveEarnScoreLive(ptrdiff_t CriticalMatchBonusOffset, FEarnScoreCallSites& OutSites);
    // 5.21 (EndMatch): AFortGameMode::SendEarnScore's score builder and grant; OutSites gets the gate and score calls.
    bool ResolveMatchEndScoresLive(FEarnScoreCallSites& OutSites);
    bool ResolveLeaveOutpostLive();
    bool ResolveOutpostMcpRequestLive();
    bool ResolveResetGameplayAttributesLive();
    bool ResolveLootTierRedirectLive();
    bool ResolvePlaylistDataTablesLive();
    // 4.2: AFortAthenaMapInfo::IsMapInfoValidForAircraft and InitializeFlightPath (Natives), from OnPlaylistDataLoaded's
    // build of the aircraft paths; false (and nothing set) where StartAircraftPhase still builds them (3.5 to 4.1).
    // FlightPathInitializedOffset: AFortGameModeAthena's bFlightPathInitialized.
    bool ResolveFlightPathLive(ptrdiff_t FlightPathInitializedOffset);
    // 3.5 to 4.5: AFortGameStateAthena::SetCurrentPlaylistId (Natives), from its log string; false where it has no
    // function of its own (5.0 to 5.21 inline it into OnGivenMatchAssignment, 5.30 strips it).
    // StormReachedFinalPositionOffset: AFortGameStateAthena's bStormReachedFinalPosition, which the id's clears (4.4, 4.5).
    bool ResolveSetCurrentPlaylistIdLive(ptrdiff_t StormReachedFinalPositionOffset);
    // AFortGameMode::OnGivenMatchAssignment and its slot in AFortGameMode's vtable, AFortGameMode::SetCurrentPlaylistName
    // and AFortGameState::StartStreamingAdditionalPlaylistLevel (Natives), from their strings. False without the first.
    // From 6.01 also AFortGameState's MatchOptions, which OnGivenMatchAssignment fills.
    // GameModeVTable: AFortGameMode's default object's.
    bool ResolveMatchAssignmentLive(void** GameModeVTable);
    // From 6.01: the slot of AFortPlayerController's ShouldDropItem (Natives), the one AFortPlayerController fills with
    // "return the answer it was given" and AFortPlayerControllerAthena with a function reading the game state's
    // GameplayMutators. GameplayMutatorsOffset: AFortGameStateAthena's GameplayMutators. False where there is none.
    bool ResolveShouldDropItemLive(ptrdiff_t GameplayMutatorsOffset);
    // From 6.01: the slot of AFortGameModeAthena's IsRespawningAllowed (Natives), the one function of its vtable that
    // both reads the game mode's GameplayMutators and tests the game state's bAllowRespawning. GameplayMutatorsOffset:
    // AFortGameModeAthena's GameplayMutators; AllowRespawningOffset and AllowRespawningMask: the byte and the bit of
    // AFortGameStateAthena's bAllowRespawning. False where there is none, or more than one.
    bool ResolveIsRespawningAllowedLive(ptrdiff_t GameplayMutatorsOffset, ptrdiff_t AllowRespawningOffset, uint8_t AllowRespawningMask);
    // From 5.21: the slots of AFortGameMode::GetLootTierDataTables and GetLootPackageDataTables (Natives), the two
    // AFortGameModeAthena overrides that load the playlist's LootTierData and LootPackages and call their Super, the game
    // mode's own GetLootTierDataTables reading the game data's LootTierDataTables. GameDataLootTierDataTablesOffset:
    // UFortGameData's LootTierDataTables; PlaylistLootTierDataOffset and PlaylistLootPackagesOffset: UFortPlaylist's
    // LootTierData and LootPackages. False where they aren't found.
    bool ResolveLootDataTablesLive(ptrdiff_t GameDataLootTierDataTablesOffset, ptrdiff_t PlaylistLootTierDataOffset, ptrdiff_t PlaylistLootPackagesOffset);
    // AFortGameStateZone::OnAbilitySystemInterfaceReadyForPersistentGameplayEffects (Natives), the function holding its
    // STAT_Fort_GameStateZoneApplyingPersistentEffects scope, and UObject::GetInterfaceAddress, which its callers need for
    // an actor's IAbilitySystemInterface. False without either (1.x builds have no stat names).
    bool ResolvePersistentGameplayEffectsLive();
    bool ResolvePoiVolumeLive();
    // AFortAthenaMapInfo::PickSupplyDropLocation and SpawnSupplyDropAtLocation (Natives), from 3.3. LlamaClassOffset:
    // AFortAthenaMapInfo's LlamaClass.
    bool ResolveSupplyDropSpawnLive(ptrdiff_t LlamaClassOffset);
    bool ResolveQuestStatEvents();
    // When ResolveQuestStatEvents found the stat events stripped: the call sites that call the shared empty function with
    // SendStatEventWithTags' typed wrapper's arguments (the quest manager, type, target, three tag containers, count, flush).
    const std::vector<uint8_t*>& StrippedQuestStatEventCalls();
    // And the calls to the shared empty function in place of the stripped static team sender (6.00's TeamInteract): the
    // event's type, the instigating player controller, then the target, three tag containers, count and flush.
    const std::vector<uint8_t*>& StrippedQuestTeamStatEventCalls();
    bool ResolveAthenaHeroLive();
    uint8_t* AircraftPhasePlayerCleanupCall(ptrdiff_t QuickBarsOffset);
    bool ResolveAthenaMenuHero();
    bool ResolveAthenaHUDPawnBinding();
    bool ResolveMcpProfileQuery();
    bool ResolveDBNOReviveLive(ptrdiff_t IsDBNOOffset, ptrdiff_t QuickBarsOffset);
    // AFortPlayerPawn::StartDBNO's slot in AFortPlayerPawnAthena's vtable (the function that sets bIsDBNO), or -1.
    int32_t StartDBNOIndex(ptrdiff_t IsDBNOOffset);
    // UStatManager::GetStatValue, which the end-of-match screen's stats are read with (Natives::UStatManager_GetStatValue).
    bool ResolveMatchStatsLive();
    // From 5.30: Score Royale's scoring of a tagged event on the player state (Natives::
    // AFortPlayerStateAthena_HandleScoringEvent), the last call of the coin's and the supply drop's scoring natives.
    bool ResolveScoringEventLive();
    // The slot of AFortPlayerControllerAthena's pawn-death handler when the build strips it (1.9.1), else -1.
    int32_t StrippedAthenaPawnDiedIndex();
    // The slot of AFortPlayerControllerAthena's PrepareDeathReport override when the build strips its Athena game mode
    // part (1.9.1, 1.10), else -1. KillerPlayerStateOffset: PlayerDeathReport's KillerPlayerState in the controller.
    int32_t StrippedAthenaPrepareDeathReportIndex(ptrdiff_t KillerPlayerStateOffset);
    bool ResolveAthenaPawnDiedLive(ptrdiff_t PlaceOffset, ptrdiff_t SecondsAliveOffset, ptrdiff_t DeathCauseOffset, ptrdiff_t DeathInfoDBNOOffset, ptrdiff_t TeamMembersOffset);
    // From 6.01: the player state's own store of a death and FDeathInfo::DeathCircumstance's value for a pawn (Natives),
    // the two calls the pawn's DBNO handling makes for the FDeathInfo it builds on the stack, after ToDeathCause (which
    // ResolveAthenaPawnDiedLive found). The offsets are the player state's: its DeathInfo, and DeathInfo's DeathCause,
    // bInitialized and DeathCircumstance. False unless the store is found; the circumstance is looked for with it.
    bool ResolveSetDeathInfoLive(ptrdiff_t DeathInfoOffset, ptrdiff_t DeathCauseOffset, ptrdiff_t InitializedOffset, ptrdiff_t DeathCircumstanceOffset);
    uint8_t* PawnDiedInventoryDropCall();
    // From 6.00 (Shadow Stones): AFortPlayerControllerGameplay::StartGhostMode's call to the stripped
    // UFortKismetLibrary::GiveItemToInventoryOwner, which gives the controller the ghost mode item. Null on builds without
    // StartGhostMode.
    uint8_t* StartGhostModeGiveItemCall();
    // From 6.01 (Disco Domination): the call to the stripped UFortKismetLibrary::K2_SpawnPickupInWorld in
    // AFortAthenaMutator_Disco's AFortGameModeAthena_OnPawnDeathBeforeRespawning, which drops a share of a dead player's
    // ammo while the mode respawns its players. SpawnPickupInWorld: the UFunction, still with the game's exec. Null on
    // builds without the mutator or its CachedPercentAmmoToDrop.
    uint8_t* DiscoPawnDeathSpawnPickupCall(UFunction* SpawnPickupInWorld);
    // AFortPlayerPawnAthena::BeginSkydiving (Natives::AFortPlayerPawnAthena_BeginSkydiving), for Battle Royale's player
    // bots. IsSkydivingOffset: AFortPlayerPawnAthena's bIsSkydiving, which it checks first.
    bool ResolveBotSkydiveLive(ptrdiff_t IsSkydivingOffset);
    // AFortPawn::SetCurrentMovementStyle's slot (Natives::AFortPawn_SetCurrentMovementStyleIndex), for Battle Royale's
    // player bots. CurrentMovementStyleOffset: AFortPawn's CurrentMovementStyle, which it stores first.
    bool ResolveMovementStyleLive(ptrdiff_t CurrentMovementStyleOffset);
    // AFortGameMode::AttemptChangeTeamInfoForController's slot (Natives::AFortGameMode_AttemptChangeTeamInfoForControllerIndex),
    // from PlacePlayerOnTeam, for Battle Royale's player bots: they join their team as a player does.
    bool ResolveTeamChangeLive();
    // AFortTeamInfo::RemoveFromTeam (Natives::AFortTeamInfo_RemoveFromTeam), from its log line, for Battle Royale's player
    // bots: one leaving the match leaves its team as a player changing teams does.
    bool ResolveTeamRemoveLive();
    // UOnlineEngineInterface::CreateUniquePlayerId's slot (Natives::UOnlineEngineInterface_CreateUniquePlayerIdIndex) in
    // the engine's online interface object's vtable, for Battle Royale's player bots' ids.
    bool ResolveCreateUniquePlayerIdLive(void* const* OnlineEngineInterfaceVTable);
    // AGameSession::GetNextPlayerID's counter (Natives::AGameSession_NextPlayerID), from AGameSession::RegisterPlayer's
    // store to the player state's PlayerId (at PlayerIdOffset), for Battle Royale's player bots' PlayerIds.
    bool ResolveNextPlayerIDLive(ptrdiff_t PlayerIdOffset);
    // AFortTeamPrivateInfo::SetHealthAndShield (Natives::AFortTeamPrivateInfo_SetHealthAndShield), from the Athena game
    // mode's ReplicateHealthAndShield: its 100 MaxShield store (MaxShieldOffset, the player state's) and then its call on
    // the player state's PlayerTeamPrivate (PlayerTeamPrivateOffset). For Battle Royale's player bots' squad bar health.
    bool ResolveTeamPrivateHealthAndShieldLive(ptrdiff_t PlayerTeamPrivateOffset, ptrdiff_t MaxShieldOffset);
    bool ResolveReplicationDriverLive();
    // The replication graph's per-connection actor infos (Natives::UNetReplicationGraphConnection_ActorMapOffset and
    // FPerConnectionActorInfoMap_RemoveByKey), for UReplicationGraph::RemoveActorInfoFromConnections (CoreV2 #411).
    // GraphVTable is the live graph's. Only where every piece is where 6.00, 6.01, 6.02, 6.10, 6.20 and 6.21 have it:
    // CL-4395664 to CL-4526925, UNetReplicationGraphConnection::NotifyActorChannelCleanedUp's ChannelMap lookup, its
    // remove and the stores that clear the info it found (6.00 to 6.10's 0x20 byte info or 6.20's 0x30 byte one), and
    // UReplicationGraph::NotifyActorTearOff's lookup in the map 0x50 before that one and its tear-off flag store at
    // that info's flags. Anything else leaves the build as it is and says so once.
    bool ResolveReplicationGraphActorInfoLive(void** GraphVTable);
    bool ResolveDecoSpawnedLive();
    bool ResolveScoreBuildingConstructionLive();
    bool ResolveGetPoiTagsForLive();
    bool ResolveBuildingRestorationLive(ptrdiff_t EditingPlayerOffset);
    bool OnlyLoadsEmoteAnimation(uintptr_t Implementation, ptrdiff_t AnimationOffset);
    // UAbilitySystemComponent::GiveAbilityAndActivateOnce and FGameplayAbilitySpecHandle::GenerateNewHandle's counter
    // (Natives::UAbilitySystemComponent_GiveAbilityAndActivateOnce, Natives::FGameplayAbilitySpecHandle_GHandleID): the
    // engine's, for every spec Core gives (restored emotes, sprays and rides, Battle Royale's player bots' abilities).
    bool ResolveAbilitySpecLive();
    bool ResolveEmoteLive(ptrdiff_t MyFortPawnOffset, ptrdiff_t StasisModeOffset);
    bool ResolvePersonalVehicleLive(UFunction* IsPersonalVehicleAvailable, ptrdiff_t MainMcpProfileOffset);
    bool ResolveServerMoveLive();
    uint8_t* SharedEmptyCallInImplementation(UFunction* Function);
    bool ResolveInventoryLive();
    bool ResolveBackpackLive(ptrdiff_t PrimaryPickupItemEntryOffset, ptrdiff_t WorldItemEntryOffset);
    bool ResolveWorldItemCountLive(ptrdiff_t RegenCooldownOffset, ptrdiff_t ItemEntryOffset);
    bool ResolveItemEntryStateLive();
    bool ResolveNeverPersistItemDefinitionLive();
    bool ResolveDisassemblyLive(UFunction* ServerDisassembleInventoryItem, UObject* PlayerControllerDefault);
    std::vector<uint8_t*> GoalManagerGetCurrentCalls(void** GoalManagerVTable, void** DirectorVTable, int32_t GoalManagerSize, int32_t DirectorSize, int32_t ActorSize, int32_t GetWorldIndex, int32_t EncounterAssignmentsOffset, uintptr_t& OutStub, std::vector<uint8_t*>& OutDirectorCalls, std::vector<uint8_t*>& OutEncounterCalls);
    uint8_t* FirstCallInImplementation(UFunction* Function, uintptr_t Target);
    uint8_t* ImplementationCall(UFunction* Function);
    bool ResolveZoneEndLive(ptrdiff_t MissionInfoOffset, ptrdiff_t ZoneFinishedOffset, ptrdiff_t ZoneCompletedOffset, ptrdiff_t EndOfZoneRemainTimeOffset, ptrdiff_t ActiveSpawnPadOffset);
    bool ResolveInventoryOwnerLive(UFunction* ServerRemoveInventoryItem, UObject* PlayerControllerDefault, ptrdiff_t WorldInventoryOffset);
    uint8_t* QuickBarChangedWorldManagerCall();
    uint8_t* ExplorationActorFinderCall(ptrdiff_t DistanceRangeMinOffset, ptrdiff_t DistanceRangeMaxOffset);
    uint8_t* AbilityEndCleanupCall(ptrdiff_t AbilityGrantedWeaponItemOffset);
    bool ResolveConditionalFoundationQuotaLive(ptrdiff_t QuotaOffset, FConditionalFoundationQuotaSites& OutSites);
    bool ResolvePlayerSpawnQueriesLive(FPlayerSpawnQuerySites& OutSites);
    // AFortPlayerState's home actor setter (Natives::AFortPlayerState_SetHomeActor), the call in
    // AFortGameMode::SpawnDefaultPawnFor_Implementation that both reads and writes the player state's HomeActor.
    bool ResolveSetHomeActorLive(ptrdiff_t HomeActorOffset);
    bool ResolveAbilityWeaponLive(ptrdiff_t CurrentWeaponListOffset, ptrdiff_t LastEquippedWeaponGuidOffset);
    bool ResolveWeaponAmmoLive(ptrdiff_t AmmoCountOffset, ptrdiff_t ItemEntryGuidOffset);
    bool IsStubFunction(uintptr_t Function);
    bool IsGetWorldStub(uintptr_t Function, uintptr_t Super);
    bool ResolveLootLive();
    bool ResolveBuildingLootLive();
    bool ResolveHarvestLive(ptrdiff_t AllowResourceDropOffset, uint8_t AllowResourceDropMask);
    bool ResolveClearAnimatingMIDsLive(ptrdiff_t ClearMIDOffset, uint8_t ClearMIDMask, ptrdiff_t AnimatingMIDsOffset);
    bool ResolveLootQuotaLive(ptrdiff_t MaximumQuotaLevelBoundOffset);
    bool ResolveWorldLootLive(ptrdiff_t DefaultLootQuotaCategoryOffset, ptrdiff_t LootDataOffset, ptrdiff_t ConstructTileWorldOffset, uint8_t ConstructTileWorldMask, FWorldLootSites& OutSites);
    bool ResolveSpawnGroupUpgradeLive(ptrdiff_t AvailableUpgradesOffset, ptrdiff_t ActiveSpawnGroupsOffset, ptrdiff_t TotalGroupCostOffset, ptrdiff_t PawnUpgradesOffset, ptrdiff_t ModifiersForAllPawnsOffset, FSpawnGroupUpgradeSites& OutSites);
    bool ResolveDurabilityLive(UFunction* ServerModDurability, UObject* PlayerControllerDefault, ptrdiff_t EntryDurabilityOffset, ptrdiff_t PendingDestroyDueToDurabilityOffset, uint8_t PendingDestroyDueToDurabilityMask);
    bool ResolvePickupCanInteractLive(ptrdiff_t PickedUpOffset);
    // IFortInteractInterface::ServerOnAttemptInteract's interface slot and UObject::GetInterfaceAddress.
    bool ResolveServerOnAttemptInteractLive();
    // From 3.5: the delegate instance vtable of ABuildingItemCollectorActor::CheckDepositGoalReached's output timer
    // (Natives::ABuildingItemCollectorActor_GrantOutputDelegateVTable), from CollectorDefault's interact.
    bool ResolveItemCollectorGrantOutputLive(UObject* CollectorDefault, UClass* InteractInterface);
    // From 5.40 (The Getaway): AFortSpecialActorReplicationInfo::AddActorToReplicationList and the constructor and
    // destructor of the FSpecialActorInitData it takes, from AFortAthenaExitCraftSpawner::RegisterSpawnerSpecialActor
    // (the "%s_%s" function that stores Add's id into SpawnerSpecialActorIDOffset), and RemoveActorFromReplicationList,
    // from AFortAthenaExitCraft's departure (the call that passes CraftSpecialActorIDOffset); both load the list off the
    // game state's SpecialActorDataOffset. Logged once; false where any is missing.
    bool ResolveSpecialActorReplicationLive(ptrdiff_t SpawnerSpecialActorIDOffset, ptrdiff_t CraftSpecialActorIDOffset, ptrdiff_t SpecialActorDataOffset);
    // From 6.00: ABuildingFoundation::SelectAndSetupMyBuildingLevel and the bTransformSet and bPostUpdateCalled bits, off
    // the foundation's PostUpdate (its vtable's ABuildingSMActor::PostUpdate slot), which tests the first where it reads
    // DynamicFoundationType and sets the second last. False on builds without dynamic foundations.
    bool ResolveBuildingFoundationLive(void** FoundationVTable, ptrdiff_t DynamicFoundationTypeOffset);
    // From 5.40: the `lea rax, [shared empty function]` sites that bind the stripped method of AFortAthenaExitCraftSpawner's
    // ExitCraftTimerHandle timer (the delegate instance's method, mov [reg+10h], rax, before FTimerManager::
    // InternalSetTimer): in StartExitCraftSpawnTimer's body and in the spawner's BeginPlay (its slots that differ from
    // ActorVTable's and read ExitCraftInfoOffset). Empty where none is found.
    std::vector<uint8_t*> ExitCraftSpawnTimerBinds(UFunction* StartExitCraftSpawnTimer, void** SpawnerVTable, void** ActorVTable, ptrdiff_t ExitCraftInfoOffset);
}
