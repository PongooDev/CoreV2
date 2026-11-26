#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Templates/SharedPointerInternals.h"

class FOutputDevice;
class FPlatformFileManager;
class IPlatformFile;
class FPakPlatformFile;
class FLinkerInstancingContext;
class UFortAssetManager;
class IModularFeatures;
class FThreadHeartBeat;
class UFortAbilitySystemComponent;
class UAthenaAIPerceptionManager;
class UAthenaMarkerComponent;
class AAthenaAIController;
struct FGameplayAbilityActivationInfo;
struct FGameplayAbilityRepSharedAnim;
struct FWeakObjectPtr;
class UNavigationSystem;
class ANavigationData;
class AFortNavigationGraph;
class UBuildingStructuralSupportSystem;
struct FNavGraphCells;
class FOutputDeviceRedirector;
class AActor;
class AWorldSettings;
class FNetGUIDCache;
class FNetworkObjectList;
class FObjectReplicator;
class FOutBunch;
class AActor;
class AController;
class AFortGameMode;
class AFortInventory;
class AFortWeapon;
class UFortWorldItem;
class UFortWorldItemDefinition;
struct FGuid;
struct FFortCreatePickupData;
class AFortGameState;
class AFortGameStateZone;
class UFortMissionInfo;
struct FFortMissionManagerRecord;
class AFortGameModeZone;
class AFortLobbyBeaconHost;
class AFortPlayerController;
class AFortPlayerControllerAthena;
class AFortPlayerPawnAthena;
class AFortPlayerStateAthena;
class AFortPlayerStateZone;
class AFortTeamInfo;
class AFortTeamPrivateInfo;
class AFortGameModeAthena;
class AFortAthenaMapInfo;
class AFortGameStateAthena;
class AFortAthenaSupplyDrop;
class AFortSpecialActorReplicationInfo;
struct FSpecialActorInitData;
class ABuildingFoundation;
struct FFortBuildingRandomList;
class APlayerState;
class APlayerController;
class AFortPlayerState;
class AFortPlayerPawn;
class UFortItemDefinition;
class UFortWeaponItemDefinition;
class UFortSchematicItem;
class UFortQuestManager;
class UFortRegisteredPlayerInfo;
class UGameInstance;
struct FGameplayTagContainer;
class AFortWorldManager;
class APawn;
class FGameServiceMcp;
class FOnlineSubsystemImpl;
class FOnlineSubsystemMcp;
class FUniqueNetId;
class IOnlineSession;
class IHttpRequest;
class UMcpProfile;
class UMcpProfileGroup;
struct FMcpServiceConfig;
struct FProfileHttpRequest;
class FString;
struct FUniqueNetIdRepl;
class UActorChannel;
class UChannel;
class UNetConnection;
struct FNetViewer;
class FTextLocalizationManager;
class IConsoleManager;
template <typename T>
class TArray;
class FScriptArray;
class UGameViewportClient;
class UFortUITeamInfo;
struct FFortTeamMemberInfo;
class UAthenaHUDContext;
class ULocalPlayer;
class FUObjectArray;
class UClass;
class UEngine;
class UFunction;
class UGameEngine;
class UGameInstance;
class UNetDriver;
class UObject;
class UAbilitySystemComponent;
class IAbilitySystemInterface;
class UFortHero;
class UFortGadgetItemDefinition;
class UFortAbilityKit;
class UFortHomebaseManager;
class UFortMcpProfileCampaign;
class UFortPersonalVehicleItem;
struct FFortSavedModeLoadout;
class UGameplayAbility;
class UScriptStruct;
struct FGameplayAbilitySpec;
struct FGameplayAbilitySpecHandle;
struct FActiveGameplayEffectHandle;
struct FGameplayEventData;
class UWorld;
struct FActorSpawnParameters;
struct FFrame;
struct FFastArraySerializer;
struct FFortItemEntry;
struct FRotator;
struct FVector;
struct FVector2D;
struct FDeathInfo;
class AFortPawn;
class AFortPickup;
class UFortItemDefinition;
class UProperty;
struct FTransform;
struct FLevelCollection;
struct FLogCategoryBase;
enum class ELevelCollectionType;
struct FName;
struct FURL;
struct FWorldContext;

namespace Natives
{
    struct FMalloc;

    inline FUObjectArray* GUObjectArray = nullptr;
    inline void** GNames = nullptr;
    inline FMalloc** GMalloc = nullptr;
    inline UEngine** GEngine = nullptr;
    inline UWorld** GWorld = nullptr;
    inline bool* GIsClient = nullptr;
    inline bool* GIsServer = nullptr;
    inline bool* PRIVATE_GIsRunningCommandlet = nullptr;
    inline bool* GIsRunning = nullptr;
    inline bool* GIsRequestingExit = nullptr;
    inline uint32_t* GGameThreadId = nullptr;
    inline bool* GIsGameThreadIdInitialized = nullptr;

    inline FName* (*FName_Construct)(FName* This, const wchar_t* Name, int32_t FindType) = nullptr;
    inline void (*FName_AppendString)(const FName* This, FString& Out) = nullptr;
    inline FString* (*FName_ToString)(const FName* This, FString* Out) = nullptr;
    // From 8.20 (UE 4.23's sparse delegates; Finder::ResolveSparseDelegateStorageLive): FSparseDelegateStorage::Add (8.20
    // 0x1CC06D0; Delegate is the FScriptDelegate passed by value, the caller's copy) and Remove (0x1CD5DF0). Each
    // returns whether the delegate is still bound.
    inline bool (*FSparseDelegateStorage_Add)(const UObject* DelegateOwner, FName DelegateName, const void* Delegate) = nullptr;
    inline bool (*FSparseDelegateStorage_Remove)(const UObject* DelegateOwner, FName DelegateName, const UObject* UserObject, FName FunctionName) = nullptr;
    // IModularFeatures::Get (Core's Features/ModularFeatures.cpp: the static FModularFeatures), from the game's own lookups
    // of its "VoiceChatToken" feature (Finder::ResolveModularFeaturesLive). 7.00 0x16F3FC0, 6.31 0x1703DE0, 7.10 0x174B6F0.
    inline IModularFeatures& (*IModularFeatures_Get)() = nullptr;
    // FThreadHeartBeat::GetNoInit, SuspendHeartBeat and ResumeHeartBeat (Finder::ResolveThreadHeartBeat). 9.10 CL-6573057
    // 0x1D7DF90, 0x1D8B200, 0x1D88E00; 9.01 0x1CE1F90, 0x1CEEFD0, 0x1CECBC0; 3.3 0x11FBA90, 0x120B100, 0x1207340.
    inline FThreadHeartBeat* (*FThreadHeartBeat_GetNoInit)() = nullptr;
    inline void (*FThreadHeartBeat_SuspendHeartBeat)(FThreadHeartBeat* This, bool bAllThreads) = nullptr;
    inline void (*FThreadHeartBeat_ResumeHeartBeat)(FThreadHeartBeat* This, bool bAllThreads) = nullptr;

    inline int (*CRT_rand)() = nullptr;
    inline float (*FMath_SRand)() = nullptr;
    inline int32_t (*FUObjectArray_AllocateSerialNumber)(FUObjectArray* This, int32_t Index) = nullptr;
    // 5.0 (UE 4.21): AllocateSerialNumber is inlined into FWeakObjectPtr::operator=(const UObject*), called instead.
    inline void (*FWeakObjectPtr_Assign)(FWeakObjectPtr* This, const UObject* Object) = nullptr;
    inline FOutputDeviceRedirector* (*GetGlobalLogSingleton)() = nullptr;
    inline void (*FOutputDeviceRedirector_AddOutputDevice)(FOutputDeviceRedirector* This, FOutputDevice* OutputDevice) = nullptr;

    inline const wchar_t* (*FPlatformMisc_GameDir)() = nullptr;
    // 8.40+ (Finder::ResolvePakPlatformFileMount): FPlatformFileManager::Get and FindPlatformFile (8.40 0x21466B0,
    // 0x2144A70), FPakPlatformFile::Mount (0x2883A10) and the offset of FPakPlatformFile::bSigned (+0x30), which Mount
    // hands to every FPakFile it makes.
    inline FPlatformFileManager& (*FPlatformFileManager_Get)() = nullptr;
    inline IPlatformFile* (*FPlatformFileManager_FindPlatformFile)(FPlatformFileManager* This, const wchar_t* Name) = nullptr;
    inline bool (*FPakPlatformFile_Mount)(FPakPlatformFile* This, const wchar_t* InPakFilename, uint32_t PakOrder, const wchar_t* InPath) = nullptr;
    inline ptrdiff_t FPakPlatformFile_bSigned = -1;
    // 8.40+ (Finder::ResolvePakPlatformFileVTable): FPakPlatformFile's vtable (8.40 0x4899308), found from GetName's
    // "PakFile" at slot 14. CoreRedirect takes over the OpenRead slot (25) on it to turn pak signing off: Initialize
    // (slot 4) and InitializeAfterSetActive (slot 5) are called on the concrete type here, not through the vtable, so a
    // vtable swap can't catch them, whereas file operations always go through IPlatformFile* -- and OpenRead is the one
    // the engine routes through the pak platform file while the config system is still reading the inis.
    inline void** FPakPlatformFile_VTable = nullptr;
    // 9.00+ (Finder::ResolvePakPrecacher): FPakPrecacher::PakPrecacherSingleton (9.00 0x5CAA380), which
    // FPakPlatformFile::InitializeNewAsyncIO (slot 8, 0x2A285F0) makes, and the offset of the precacher's SigningKey
    // (TSharedPtr object, +0x290), set from GetPakSigningKey() by its constructor (0x2A1BFA0). RegisterPakFile
    // (0x2A2DE80) loads a pak's FPakSignatureFile and the read callback (0x2A1D810) starts DoSignatureCheck only while
    // it is set.
    inline uint8_t** FPakPrecacher_PakPrecacherSingleton = nullptr;
    inline ptrdiff_t FPakPrecacher_SigningKey = -1;
    inline const wchar_t* (*FPlatformProcess_UserName)(bool bOnlyAlphaNumeric) = nullptr;
    inline bool (*FPackageName_FindPackagesInDirectory)(TArray<FString>& OutPackages, const FString& RootDir) = nullptr;
    inline FString* FPackageName_MapPackageExtension = nullptr;
    inline IConsoleManager** IConsoleManager_Singleton = nullptr;
    inline void (*IConsoleManager_SetupSingleton)() = nullptr;
    inline FTextLocalizationManager** GTextLocalizationManager = nullptr;
    inline FTextLocalizationManager& (*FTextLocalizationManager_Get)() = nullptr;
    inline void* (*FTextLocalizationManager_FindDisplayString)(const FTextLocalizationManager* This, void* Result, const FString* Namespace, const FString* Key, const FString* SourceString) = nullptr;
    inline void* UConsole_RegisterConsoleAutoCompleteEntries = nullptr;

    inline void (*FMsg_Logf_Internal)(const char* File, int32_t Line, const FName& Category, int32_t Verbosity, const wchar_t* Format, ...) = nullptr;
    inline void (*FLogCategoryBase_FLogCategoryBase)(FLogCategoryBase* This, const wchar_t* CategoryName, uint8_t InDefaultVerbosity, uint8_t InCompileTimeVerbosity) = nullptr;
    inline void (*FDebug_AssertFailed)(const char* Expr, const char* File, int32_t Line, const wchar_t* Format, ...) = nullptr;
    inline void (*LowLevelFatalErrorHandler)(const char* File, int32_t Line, const wchar_t* Format, ...) = nullptr;

    inline UObject* (*StaticFindObject)(UClass* Class, UObject* InOuter, const wchar_t* Name, bool ExactClass) = nullptr;
    inline UObject* (*StaticLoadObject)(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename, uint32_t LoadFlags, void* Sandbox, bool bAllowObjectReconciliation, const FLinkerInstancingContext* InstancingContext) = nullptr;
    inline UObject* (*StaticConstructObject_Internal)(UClass* Class, UObject* InOuter, FName Name, int32_t SetFlags, int32_t InternalSetFlags, UObject* Template, bool bCopyTransientsFromClassDefaults, void* InstanceGraph, bool bAssumeTemplateIsArchetype) = nullptr;
    inline void (*CollectGarbage)(int32_t KeepFlags, bool bPerformFullPurge) = nullptr;

    inline const wchar_t* (*FCommandLine_Get)() = nullptr;
    inline bool (*FParse_Param)(const wchar_t* Stream, const wchar_t* Param) = nullptr;

    inline void (*GetSourceIniHierarchyFilenames)(const wchar_t* InBaseIniName, const wchar_t* InPlatformName, const wchar_t* EngineConfigDir, const wchar_t* SourceConfigDir, void* OutHierarchy, bool bRequireDefaultIni) = nullptr;
    inline bool (*FConfigCacheIni_GetBool)(void* This, const wchar_t* Section, const wchar_t* Key, bool& Value, const FString& Filename) = nullptr;
    inline void* (*FConfigCacheIni_Find)(void* This, const FString& Filename, bool bCreateIfNotFound) = nullptr;
    inline void (*FConfigFile_CombineFromBuffer)(void* This, const FString& Buffer) = nullptr;
    inline void** GConfig = nullptr;
    inline const FString* GEngineIni = nullptr;
    inline const FString* GGameIni = nullptr;
    inline int32_t (*FConfigCacheIni_GetArray)(void* This, const wchar_t* Section, const wchar_t* Key, TArray<FString>& OutArray, const FString& Filename) = nullptr;
    // Sorts an FFortKeyChain's keys and sets its Hash from them (8.00 0xFA6A40), which FFortContentEncryptionManager::
    // MergeInKeyChain compares with its own before merging anything.
    inline void (*FFortKeyChain_UpdateHash)(void* KeyChain) = nullptr;
    inline void** FOnlineVoiceImpl_VTable = nullptr;
    inline int32_t FOnlineVoiceImpl_InitIndex = -1;
    inline int32_t FOnlineVoiceImpl_SerializeRemotePacketIndex = -1;
    inline int32_t FOnlineVoiceImpl_RegisterLocalTalkerIndex = -1;
    inline int32_t FOnlineVoiceImpl_GetLocalPacketIndex = -1;
    inline int32_t FOnlineVoiceImpl_TickIndex = -1;
    inline int32_t FVoicePacket_GetSenderIndex = -1;
    inline ptrdiff_t APlayerController_VoiceHandshakeCompletedOffset = -1;
    // AFortPlayerControllerZone's unreflected TMap<const UFortGadgetItemDefinition*, FGuid>: each applied gadget with
    // tracked attributes and its item (7.20 +0x1DF8), from Finder::GadgetTrackedAttributeBind; on 3.5-6.10 the reflected
    // FGuid GadgetTrackedAttributeItemInstanceId (5.10 +0x1A80). -1 when not found.
    inline ptrdiff_t AFortPlayerControllerZone_GadgetTrackedAttributeItemInstanceIds = -1;
    // 7.30+: that member is a TSet<FGuid> of the items (Finder::GadgetTrackedAttributeBind's bItemGuidSet).
    inline bool AFortPlayerControllerZone_bGadgetTrackedAttributeItemGuidSet = false;
    // UFortGadgetItemDefinition::ApplyGadgetData(InventoryOwner, Item, bArg) (9.00 CL-6337466 0x14F62B0, 8.51 0x144ED60,
    // 8.50 0x1450510), from Finder::GadgetTrackedAttributeBind. InventoryOwner is the controller's
    // IFortInventoryOwnerInterface. Its ability set (ApplyGadgetItemAbilitySet, 9.00 0x17D5490) and attribute set
    // (ApplyGadgetItemAttributeSet, 0x17D5620) each skip a gadget item they applied already; both need the controller's pawn.
    inline void (*UFortGadgetItemDefinition_ApplyGadgetData)(UObject* This, void* InventoryOwner, UFortWorldItem* Item, bool bArg) = nullptr;
    // UFortGadgetItemDefinition::RemoveGadgetData(InventoryOwner, Item) (9.00 0x150B8D0, 8.51 0x1464F50), the same finder:
    // ApplyGadgetData's undo, through RemoveGadgetItemAbilitySet (9.00 0x17F3430) and RemoveGadgetItemAttributeSet
    // (0x17F3570, which drops the item from the controller's set of applied items).
    inline void (*UFortGadgetItemDefinition_RemoveGadgetData)(UObject* This, void* InventoryOwner, UFortWorldItem* Item) = nullptr;
    inline FString* (*UGameMapsSettings_GetGameDefaultMap)(FString* Result) = nullptr;
    inline const FString* (*UGameMapsSettings_GetGlobalDefaultGameMode)() = nullptr;

    inline void (*UGameInstance_StartGameInstance)(UGameInstance* This) = nullptr;
    inline bool (*UGameInstance_IsDedicatedServerInstance)(const UGameInstance* This) = nullptr;
    inline bool (*UObject_CanCreateInCurrentContext)(UObject* Template) = nullptr;
    inline UClass** (*AFortGameMode_GetGameSessionClass)(const AFortGameMode* This, UClass** Result) = nullptr;
    inline APawn* (*AFortGameMode_SpawnDefaultPawnFor)(AFortGameMode* This, AController* NewPlayer, AActor* StartSpot) = nullptr;
    inline APawn* (*AFortGameModeZone_SpawnDefaultPawnFor)(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot) = nullptr;
    // What AFortGameMode::SpawnDefaultPawnFor_Implementation binds a player's start spot with when they have no home
    // actor yet (6.02 0x1244330, 6.10 0x128B260, 6.20 0x130D950): Finder::ResolveSetHomeActorLive.
    inline void (*AFortPlayerState_SetHomeActor)(AFortPlayerState* This, AActor* NewHomeActor) = nullptr;
    inline AActor* (*AFortGameMode_PickPlayerSpawnActor)(AFortGameMode* This, AController* Player, UClass* PawnClass, TArray<AActor*>* Candidates, float DangerExclusionRadius, float ObjectiveExclusionRadius) = nullptr;
    inline int32_t AFortGameMode_GetFriendlyPlayerPawnsIndex = -1;
    inline void (*AFortGameMode_FinishWorldInitialization)(AFortGameMode* This, AFortWorldManager* WorldManager) = nullptr;
    inline void (*AFortWorldManager_OnWorldRecordQueryComplete)(AFortWorldManager* This) = nullptr;
    inline void (*AFortLobbyBeaconHost_SetWorldRecordLoaded)(AFortLobbyBeaconHost* This, bool bInWorldRecordLoaded) = nullptr;
    inline bool (*AFortPlayerController_InitializeMcpProfile)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    inline void (*AFortPlayerController_TrySetHeroImplementation)(AFortPlayerController* This, UFortHero* NewHero, bool bSendToServer, bool bUnused) = nullptr;
    inline void (*AFortPlayerState_SetHeroType)(AFortPlayerState* This, UFortHero* Hero) = nullptr;
    // From 1.9 (1.9 0x87A800, 2.5 0xE4FAE0, 3.0 0xE75BB0); null on 1.1 to 1.8.2, which don't have it.
    inline void (*AFortPlayerState_ApplyCharacterCustomization)(AFortPlayerState* This, AFortPlayerPawn* Pawn) = nullptr;
    inline FFortSavedModeLoadout* (*UFortMcpProfileCampaign_GetModeLoadout)(const UFortMcpProfileCampaign* This, FFortSavedModeLoadout* Result, FString LoadoutName) = nullptr;
    inline IAbilitySystemInterface* (*UFortRegisteredPlayerInfo_GetAbilitySystemInterface)(const UFortRegisteredPlayerInfo* This) = nullptr;
    inline void (*UFortHomebaseManager_GetSelectableGadgetItems)(const UFortHomebaseManager* This, const UFortMcpProfileCampaign* AccountProfile, IAbilitySystemInterface* AbilitySystem, FScriptArray& OutItems) = nullptr;
    // Null from 5.0, which inlines it into GetSelectableGadgetItems (Core then does it from GetOwnedNodes).
    inline void (*UFortHomebaseManager_GetOwnedNodeAbilityKits)(const UFortHomebaseManager* This, const UFortMcpProfileCampaign* AccountProfile, TArray<UFortAbilityKit*>& OutKits) = nullptr;
    inline TArray<FName>* (*UFortHomebaseManager_GetOwnedNodes)(const UFortHomebaseManager* This, TArray<FName>* Result, const UFortMcpProfileCampaign* McpProfile) = nullptr;
    // 6.30+ (no skill tree, no GetOwnedNodes): the account's items of an EFortItemType that are UFortHomebaseNodeItems
    // with a definition (6.31 0xF8F210; OutItems is emptied first), and a node item's ability kits, those of its
    // FHomebaseNodeLevels up to its level (0x10679D0, returned by value). Both are what GetSelectableGadgetItems calls.
    inline void (*UFortMcpProfile_GetHomebaseNodeItemsByType)(const UFortMcpProfileCampaign* This, uint8_t ItemType, TArray<UObject*>& OutItems) = nullptr;
    inline TArray<UFortAbilityKit*>* (*UFortHomebaseNodeItem_GetAbilityKits)(const UObject* This, TArray<UFortAbilityKit*>* Result) = nullptr;
    inline void (*UFortGlobals_ApplyHeroEffectsAndAbilities)(UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles) = nullptr;
    // 7.00 (CL-4629139) on: ApplyHeroEffectsAndAbilities takes the account's UFortCampaignHeroLoadoutItem first (null for
    // a player with none), so the hero is the second argument and the ability system the third (CoreV2 #472).
    inline bool UFortGlobals_ApplyHeroEffectsAndAbilitiesTakesHeroLoadout = false;
    // 8.00 (CL-5203069) on: AFortGameState::ApplyHomebaseEffectsOnPlayerSetup (0x1053430) calls ApplyHeroEffectsAndAbilities
    // only in Save the World (its subgame test, ESubGame::Campaign, before the call at 0x10539E8); 7.30 (0xFF4B8D) called it in
    // every mode. Battle Royale's players then get their generic ability set from UFortGlobals::InitializePlayerGameplayAbilities.
    inline bool UFortGlobals_ApplyHeroEffectsAndAbilitiesCampaignOnly = false;
    // 8.00 (CL-5203069) on: UFortHero::DetermineCommanderGadgetsAndItems (8.40 0x1351470): the hero loadout commander's
    // ability kit gadgets and items (OutItems is a TArray<FAbilityKitItem>). Null before the hero loadout system.
    inline void (*UFortHero_DetermineCommanderGadgetsAndItems)(const UFortHero* Hero, TArray<UFortGadgetItemDefinition*>& OutGadgets, FScriptArray& OutItems) = nullptr;
    inline void* (*FScopedPredictionWindow_FScopedPredictionWindow)(void* This, UAbilitySystemComponent* AbilitySystemComponent, void* InPredictionKey, bool InSetReplicatedPredictionKey) = nullptr;
    inline void (*FScopedPredictionWindow_Destructor)(void* This) = nullptr;
    inline bool (*UAbilitySystemComponent_InternalTryActivateAbility)(UAbilitySystemComponent* This, FGameplayAbilitySpecHandle AbilityToActivate, const void* InPredictionKey, UGameplayAbility** OutInstancedAbility, void* OnGameplayAbilityEndedDelegate, const FGameplayEventData* TriggerEventData) = nullptr;
    inline void (*UAbilitySystemComponent_MarkAbilitySpecDirty)(UAbilitySystemComponent* This, FGameplayAbilitySpec& Spec) = nullptr;
    inline void (*UAbilitySystemComponent_IncrementAbilityListLock)(UAbilitySystemComponent* This) = nullptr;
    inline void (*UAbilitySystemComponent_DecrementAbilityListLock)(UAbilitySystemComponent* This) = nullptr;
    // GiveAbilityAndActivateOnce(const FGameplayAbilitySpec&) returns its handle through Result, and the static counter
    // FGameplayAbilitySpecHandle::GenerateNewHandle takes handles from (Finder::ResolveAbilitySpecLive).
    inline FGameplayAbilitySpecHandle* (*UAbilitySystemComponent_GiveAbilityAndActivateOnce)(UAbilitySystemComponent* This, FGameplayAbilitySpecHandle* Result, const FGameplayAbilitySpec& Spec) = nullptr;
    inline int32_t* FGameplayAbilitySpecHandle_GHandleID = nullptr;
    // 3.0's AFortPlayerController::GetCurrentlySelectedPersonalVehicle (0xE31910): the MainMcpProfile's campaign loadout
    // vehicle, else its first EFortItemType::PersonalVehicle item (Finder::ResolvePersonalVehicleLive).
    inline UFortPersonalVehicleItem* (*AFortPlayerController_GetCurrentlySelectedPersonalVehicle)(const AFortPlayerController* This) = nullptr;
    inline void* SharedEmptyFunction = nullptr;
    inline void (*FortMcpConfig_UpdateServicePermissions)(void* Environment, void* Context, FMcpServiceConfig* ServiceConfig, void* Extra) = nullptr;
    // FBaseServiceMcp::GetBasicAuth: a service's client credentials, from its config's ServicePermissions. Packed builds
    // only (Finder::ResolveMcpBasicAuth), where it tells Core which objects are MCP services.
    inline void* FBaseServiceMcp_GetBasicAuth = nullptr;
    inline void (*UMcpProfileGroup_SendRequestNow)(UMcpProfileGroup* This, FProfileHttpRequest& Request) = nullptr;
    // 7.40 (CL-5046157 0xA7D080): SendRequestNow takes the HTTP request and its context credentials instead of an
    // FProfileHttpRequest, its caller having prepared the request (0xA7C970 -> 0xA69280, the profile's PrepareRequest).
    inline bool UMcpProfileGroup_SendRequestNowTakesHttpRequest = false;
    // UMcpProfile::QueryProfile(const FMcpQueryComplete& OnComplete): the profile's whole query, by whichever URL its group
    // needs (another account's: the public one). 2.5 (0x79D250) and 3.0 (0x7A1800) take FBaseUrlContext's
    // bImmediateMode too (r8b, false from every caller of the game's); a build whose QueryProfile has no third argument
    // ignores it.
    inline void (*UMcpProfile_QueryProfile)(UMcpProfile* This, const void* OnComplete, bool bImmediateMode) = nullptr;
    inline bool (*FOnlineSubsystemMcp_QueueRequestForUser)(FOnlineSubsystemMcp* This, FGameServiceMcp& Service, const FUniqueNetId& UserId, const TSharedRef<IHttpRequest>& HttpRequest, FString& OutError) = nullptr;
    inline bool (*FOnlineSubsystemMcp_QueueRequestForClient)(FOnlineSubsystemMcp* This, FGameServiceMcp& Service, const FString& ClientId, const TSharedRef<IHttpRequest>& HttpRequest, FString& OutError) = nullptr;
    inline bool (*FOnlineSubsystemImpl_IsDedicated)(const FOnlineSubsystemImpl* This) = nullptr;
    inline void (*UGameEngine_Tick)(UGameEngine* This, float DeltaSeconds, bool bIdleMode) = nullptr;
    // UGameEngine::GameViewportWindow (TWeakPtr<SWindow>); GameViewportWidget is 0x20 after it. 0 when not found.
    inline ptrdiff_t UGameEngine_GameViewportWindow = 0;
    inline void (*UFortAssetManager_SetGameFeatureState)(UFortAssetManager* This, uint8_t Feature, uint8_t State, bool bBroadcast) = nullptr;
    inline int32_t UFortAssetManager_StartInitialLoadingIndex = -1;
    inline bool (*UEngine_CreateNamedNetDriver)(UEngine* This, UWorld* InWorld, FName NetDriverName, FName NetDriverDefinition) = nullptr;
    inline UNetDriver* (*UEngine_FindNamedNetDriver)(UEngine* This, const UWorld* InWorld, FName NetDriverName) = nullptr;
    inline void (*UEngine_DestroyNamedNetDriver)(UEngine* This, UWorld* InWorld, FName NetDriverName) = nullptr;
    inline FWorldContext* (*UEngine_GetWorldContextFromWorldChecked)(UEngine* This, const UWorld* InWorld) = nullptr;
    inline void (*FOnNetworkFailure_Broadcast)(void* Delegate, UWorld* World, UNetDriver* NetDriver, int32_t FailureType, const FString& ErrorString) = nullptr;
    inline void (*UEngine_BroadcastNetworkFailure)(UEngine* This, UWorld* World, UNetDriver* NetDriver, int32_t FailureType, const FString& ErrorString) = nullptr;

    inline ULocalPlayer* (*UGameViewportClient_SetupInitialLocalPlayer)(UGameViewportClient* This, FString& OutError) = nullptr;
    inline bool (*UFortUITeamInfo_GetTeamMemberInfo)(UFortUITeamInfo* This, UFortRegisteredPlayerInfo* PlayerInfo, FFortTeamMemberInfo& OutInfo) = nullptr;
    inline void (*UAthenaHUDContext_BindLocalPawn)(UAthenaHUDContext* This) = nullptr;

    // int64 ReplicateActor() (UE 4.22's: the bits written), or bool on the builds where the flag below is false: only
    // al is defined then.
    inline int64_t (*UActorChannel_ReplicateActor)(UActorChannel* This) = nullptr;
    // Whether ReplicateActor returns the bits it wrote (3.3 on) rather than a bool (3.0 and older), as the engine's own
    // caller UDemoNetDriver::DemoReplicateActor reads it: "test rax, rax; setg" (3.3 0x208872D, 7.20 0x27ED2D9) against
    // "movzx r15d, al" (3.0 0x1FEDB48, 1.1 0x1F3CCA6). False when that caller isn't found.
    inline bool UActorChannel_bReplicateActorReturnsBits = false;
    inline void (*UActorChannel_SetChannelActor)(UActorChannel* This, AActor* InActor) = nullptr;
    inline UChannel* (*UNetConnection_CreateChannel)(UNetConnection* This, int32_t Type, bool bOpenedLocally, int32_t ChannelIndex) = nullptr;
    // 6.10 (UE 4.22's networking) names its channels and has no CreateChannel by type: CreateChannelByName(const FName&
    // ChName, EChannelCreateFlags CreateFlags, int32 ChannelIndex) (CL-4480234 0x26EDF50). Null on the builds before it.
    inline UChannel* (*UNetConnection_CreateChannelByName)(UNetConnection* This, const FName& ChName, uint32_t CreateFlags, int32_t ChannelIndex) = nullptr;
    inline FNetViewer* (*FNetViewer_FNetViewer)(FNetViewer* This, UNetConnection* InConnection, float DeltaSeconds) = nullptr;
    inline void (*AActor_CallPreReplication)(AActor* This, UNetDriver* NetDriver) = nullptr;
    inline void (*AActor_ReceiveTick)(AActor* This, float DeltaSeconds) = nullptr;
    inline UNetDriver* (*AActor_GetNetDriver)(const AActor* This) = nullptr;
    inline void (*AActor_FlushNetDormancy)(AActor* This) = nullptr;
    inline void (*AActor_SetNetDormancy)(AActor* This, int32_t NewDormancy) = nullptr;
    // AActor::Destroy(bool bNetForce, bool bShouldModifyLevel) (6.21 0x23FBBC0, 1.8.2 0x1F3E6B0): World->DestroyActor.
    // AActor::K2_DestroyActor (6.21 0xA89A80) is "mov r8b, 1; xor edx, edx; jmp" to it: Finder::ResolveActorDestroyLive.
    inline bool (*AActor_Destroy)(AActor* This, bool bNetForce, bool bShouldModifyLevel) = nullptr;
    inline void (*FNetworkObjectList_Remove)(FNetworkObjectList* This, AActor* Actor) = nullptr;
    // FNetworkObjectInfo's members after Actor: 1.8 adds a TWeakObjectPtr<AActor> after it, moving them all by 8.
    inline ptrdiff_t FNetworkObjectInfo_NextUpdateTime = 0x8;
    inline ptrdiff_t FNetworkObjectInfo_LastNetReplicateTime = 0x10;
    inline ptrdiff_t FNetworkObjectInfo_OptimalNetUpdateDelta = 0x18;
    inline ptrdiff_t FNetworkObjectInfo_LastNetUpdateTime = 0x1C;
    inline ptrdiff_t FNetworkObjectInfo_Flags = 0x20;
    inline ptrdiff_t FNetworkObjectInfo_DormantConnections = 0x28;
    inline ptrdiff_t FNetworkObjectInfo_RecentlyDormantConnections = 0x78;
    // FActorDestructionInfo's members and size: 1.10 (UE 4.19) puts a TWeakObjectPtr<ULevel> first, moving them all by 8.
    inline ptrdiff_t FActorDestructionInfo_ObjOuter = 0x0;
    inline ptrdiff_t FActorDestructionInfo_DestroyedPosition = 0x8;
    inline ptrdiff_t FActorDestructionInfo_NetGUID = 0x14;
    inline ptrdiff_t FActorDestructionInfo_PathName = 0x18;
    inline ptrdiff_t FActorDestructionInfo_StreamingLevelName = 0x28;
    inline size_t FActorDestructionInfo_Size = 0x30;
    // 4.22's networking (7.20): why the destroyed actor's channel closes (EChannelCloseReason), after StreamingLevelName.
    inline ptrdiff_t FActorDestructionInfo_Reason = -1;
    // SupportsObject(Object, const TWeakObjectPtr<UObject>* WeakObjectPtr): 1.8 (0x22B2090) reads WeakObjectPtr when it
    // isn't null, 1.7.2 (0x223CCA0) takes no such argument.
    inline bool (*FNetGUIDCache_SupportsObject)(const FNetGUIDCache* This, const UObject* Object, const void* WeakObjectPtr) = nullptr;
    // 4.2's (0x1519300) takes a fifth argument, bUseUpToDateClass, which UObject::GetArchetype passes false; the builds
    // before it take four and leave the fifth's stack slot unread.
    inline UObject* (*UObject_GetArchetypeFromRequiredInfo)(UClass* Class, UObject* Outer, FName Name, int32_t ObjectFlags, bool bUseUpToDateClass) = nullptr;
    inline AWorldSettings* (*UWorld_GetWorldSettings)(const UWorld* This, bool bCheckStreamingPersistent, bool bChecked) = nullptr;
    inline void (*UWorld_AddNetworkActor)(UWorld* This, AActor* Actor) = nullptr;
    inline AActor* (*UWorld_SpawnActor)(UWorld* This, UClass* Class, const FTransform* UserTransformPtr, const FActorSpawnParameters& SpawnParameters) = nullptr;
    inline void (*UScriptStruct_CopyScriptStruct)(const UScriptStruct* This, void* Dest, const void* Src, int32_t ArrayDim) = nullptr;
    inline void (*FFrame_Step)(FFrame* This, UObject* Context, void* const Result) = nullptr;
    inline void (*FFastArraySerializer_MarkArrayDirty)(FFastArraySerializer* This) = nullptr;
    // 5.0 (UE 4.21) inlines MarkArrayDirty: ItemMap.Reset() (TMap<int32, int32>::Reset, ItemMap at +0).
    inline void (*FFastArraySerializer_ItemMapReset)(FFastArraySerializer* This) = nullptr;
    inline FFortItemEntry* (*FFortItemEntry_FFortItemEntry)(FFortItemEntry* This, const UFortItemDefinition* InItemDefinition, int32_t InCount, int32_t InLevel, const void* InAlterations) = nullptr;
    inline void (*FFortItemEntry_SetStateValue)(FFortItemEntry* This, uint8_t StateType, int32_t IntValue) = nullptr;
    inline void (*FFortItemEntry_MarkDirty)(FFortItemEntry* This) = nullptr;
    // 1.10 adds a seventh argument, which its callers pass as 0 (older builds ignore it). Later builds up to 4.0 read two
    // more (the first is the pickup's bRandomRotation; 4.1's FFortCreatePickupData carries both): Core passes them as 0,
    // where it left them to whatever was on the stack. A build that reads fewer ignores them.
    inline AFortPickup* (*AFortPickup_CreatePickup)(UWorld* World, const FFortItemEntry& ItemEntry, const FVector* SpawnLocation, const FRotator* SpawnRotation, AFortPlayerController* OptionalOwner, UClass* OverrideClass, uint64_t Unused, uint64_t Extra0, uint64_t Extra1) = nullptr;
    // 4.1 (CL-4053532) on: CreatePickup takes its arguments as one FFortCreatePickupData (AFortPickup::CreateFromData).
    // AFortPickup_CreatePickup then holds the same function, for the finders that look for its calls.
    inline AFortPickup* (*AFortPickup_CreateFromData)(const FFortCreatePickupData& PickupData) = nullptr;
    inline void (*FFrame_StepExplicitProperty)(FFrame* This, void* const Result, UProperty* Property) = nullptr;
    inline bool (*UNetDriver_IsAdaptiveNetUpdateFrequencyEnabled)() = nullptr;
    inline void (*UNetDriver_TickFlush)(UNetDriver* This, float DeltaSeconds) = nullptr;
    inline FOutBunch* (*FOutBunch_FOutBunch)(FOutBunch* This, UChannel* InChannel, bool bClose) = nullptr;
    inline bool (*FObjectReplicator_ValidateAgainstState)(FObjectReplicator* This, const UObject* ObjectState) = nullptr;

    inline int32_t (*UWorld_InternalGetNetMode)(const UWorld* This) = nullptr;
    inline int32_t (*UWorld_AttemptDeriveFromURL)(const UWorld* This) = nullptr;
    // 5.0 (UE 4.21): AttemptDeriveFromURL is inlined into InternalGetNetMode, so UWorld_AttemptDeriveFromURL becomes the
    // original InternalGetNetMode (its trampoline), which derives from the URL when both net drivers are null.
    inline bool bInternalGetNetModeDerivesFromURL = false;
    inline FLevelCollection* (*UWorld_FindCollectionByType)(UWorld* This, ELevelCollectionType InType) = nullptr;
    inline int32_t (*UNetDriver_GetNetMode)(const UNetDriver* This) = nullptr;
    inline void (*UNetDriver_SetWorld)(UNetDriver* This, UWorld* InWorld) = nullptr;

    inline bool (*FURL_HasOption)(const FURL* This, const wchar_t* Test) = nullptr;
    inline FString* (*FURL_ToString)(const FURL* This, FString* Result, bool FullyQualified) = nullptr;

    inline int32_t ProcessEventIndex = -1;
    inline int32_t UClass_CreateDefaultObjectIndex = -1;
    inline int32_t UNetDriver_InitListenIndex = -1;
    inline int32_t UNetDriver_TickFlushIndex = -1;
    inline int32_t UNetDriver_IsServerIndex = -1;
    inline int32_t UNetDriver_ServerReplicateActorsIndex = -1;
    inline int32_t UReplicationDriver_ServerReplicateActorsIndex = -1;
    // The replication graph (Finder::ResolveReplicationGraphActorInfoLive): where a UNetReplicationGraphConnection keeps
    // its per-actor infos (ActorInfoMap's map keyed by the actor's address; -1 unknown). UE 4.21's (6.00 to 6.31) with
    // the engine's own remove-by-key of that map's type (TMap<pointer, TSharedPtr<FConnectionReplicationActorInfo>>::
    // Remove, the one NotifyActorChannelCleanedUp calls on the ChannelMap beside it).
    inline ptrdiff_t UNetReplicationGraphConnection_ActorMapOffset = -1;
    inline int32_t (*FPerConnectionActorInfoMap_RemoveByKey)(void* Map, const void* Key) = nullptr;
    // UE 4.20's (4.1 CL-4053532, 4.2 CL-4072250): the infos are the map's values (TMap<AActor*,
    // FConnectionReplicationActorInfo>), and the build has no remove-by-key of that type, only its two halves:
    // TSet::FindId (the lookup NotifyActorTearOff and NotifyActorFullyDormantForConnection make; the FSetElementId, an
    // int32 index, -1 for none, comes back through a return pointer) and TSet::Remove(FSetElementId) (the removal
    // NotifyActorChannelCleanedUp's iterator makes).
    inline int32_t* (*FPerConnectionActorInfoMap_FindId)(void* Map, int32_t* Result, const void* Key) = nullptr;
    inline void (*FPerConnectionActorInfoMap_RemoveId)(void* Map, int32_t ElementId) = nullptr;
    // UFortReplicationGraphNode_AlwaysRelevant_ForConnection's PastRelevantActors (Finder::ResolveAlwaysRelevantPastActorsLive;
    // 9.00 and 9.01 +0x278, -1 elsewhere), for ForgetDestroyedPawn (CoreV2 #671). 9.00's has a TInlineAllocator<4>
    // (the inline entries, then the heap data at +0x40 and ArrayNum at +0x48); 9.01's (CL-6428087) is a plain TArray,
    // as 21.40's.
    inline ptrdiff_t UFortReplicationGraphNode_AlwaysRelevant_ForConnection_PastRelevantActorsOffset = -1;
    inline bool UFortReplicationGraphNode_AlwaysRelevant_ForConnection_bPastRelevantActorsInline = false;
    inline int32_t UConsole_BuildRuntimeAutoCompleteListIndex = -1;
    // 8 up to UE 4.19 (2.4.2); UE 4.20 (2.5) drops five delegate instance virtuals and has GetHandle at 4.
    inline int32_t IDelegateInstance_GetHandleIndex = 8;
    // Whether IDelegateInstance_GetHandleIndex was read from the build (the client's console delegates, the server's MCP
    // request delegate) rather than left at its default.
    inline bool IDelegateInstance_GetHandleIndexResolved = false;
    // The build's NUM_DELEGATE_INLINE_BYTES: 32 (TInlineAllocator<2>) up to 4.1. 4.2 takes UE 4.21's 0 (FHeapAllocator),
    // with NUM_MULTICAST_DELEGATE_INLINE_ENTRIES 0 too, so an FDelegateBase is { Data, DelegateSize } (#303).
    inline int32_t NumDelegateInlineBytes = 32;
    inline int32_t UObject_NeedsLoadForClientIndex = -1;
    inline int32_t UObject_GetWorldIndex = -1;
    inline int32_t AFortGameMode_FinishWorldInitializationIndex = -1;
    inline int32_t UAbilitySystemComponent_InternalServerTryActiveAbilityIndex = -1;
    inline int32_t UFortWorldItemDefinition_ServerExecuteIndex = -1;
    inline void (*AFortInventory_InitializeExistingItem)(AFortInventory* This, UFortWorldItem* Item) = nullptr;
    // 1.11+: the native bool InitializeExistingItem checks before it adds an item it doesn't have (-1 before 1.11).
    inline ptrdiff_t AFortInventory_bAllowMcpItemCreation = -1;
    inline int32_t (*AFortInventory_GetOverflowAmount)(AFortInventory* This, const FFortItemEntry& ItemEntry) = nullptr;
    inline int32_t (*AFortInventory_GetMaxStackSize)(AFortInventory* This, const UFortItemDefinition* ItemDefinition) = nullptr;
    inline bool (*AFortInventory_CanStackWith)(AFortInventory* This, UFortWorldItem* Item, const FFortItemEntry& ItemEntry, float MaxDurability) = nullptr;
    // 5.0 inlines CanStackWith into the stack limit (CL-4204761 0xF142B0); these are its pieces, which Core puts back
    // together (AFortInventory::CanStackWith): the item entries' same-item test (the definition, the level and the
    // alterations; 5.0 0xF01CF0, 4.5 0xD7B320, ItemEntry first) and UFortWorldItem::BaseRegenCooldown's offset (5.0
    // 0xF46890 reads +0x1A8). Null and -1 where the build still has CanStackWith.
    inline bool (*FFortItemEntry_IsSameItem)(const FFortItemEntry& ItemEntry, const FFortItemEntry& Other) = nullptr;
    inline ptrdiff_t UFortWorldItem_BaseRegenCooldown = -1;
    inline ptrdiff_t AFortInventory_InventoryInterfaceOffset = -1;
    inline int32_t IFortInventoryInterface_GetMaxItemsIndex = -1;
    inline int32_t IFortInventoryInterface_GetNumItemsIndex = -1;
    inline bool AFortInventory_bControllerLimits = false;
    inline uintptr_t UAbilitySystemComponent_GiveAbility = 0;
    inline int32_t AFortPickup_GivePickupToIndex = -1;
    // What the build left of GivePickupTo: the shared empty function, or (1.10) its authority check and Destroy.
    inline void* AFortPickup_GivePickupToStripped = nullptr;
    // 9.10+: GivePickupTo(InventoryOwner, bDestroyPickup), the body Destroys only when its third argument is set.
    inline bool AFortPickup_GivePickupToTakesDestroyPickup = false;
    inline int32_t AFortPlayerController_SpawnDropPickupIndex = -1;
    inline void (*AFortPickup_SetPickupTarget)(AFortPickup* Pickup, AFortPawn* PickupTarget, float InFlyTime, const FVector& InStartDirection, bool bPlayPickupSound) = nullptr;
    // The pickup's IFortInteractInterface::CanInteract(const FInteractionType&) slot, in the interface's own vtable.
    inline int32_t IFortInteractInterface_CanInteractIndex = -1;
    // IFortInteractInterface::ServerOnAttemptInteract(const FInteractionType&)'s slot in the interface's own vtable.
    inline int32_t IFortInteractInterface_ServerOnAttemptInteractIndex = -1;
    // From 3.5 (Finder::ResolveItemCollectorGrantOutputLive): the vtable of the timer delegate instance that
    // ABuildingItemCollectorActor::CheckDepositGoalReached (3.5.1 0xA866C0) sets to grant a collector's output
    // (0x32CDEB8), and its Execute and ExecuteIfSafe slots (7 and 8).
    inline void** ABuildingItemCollectorActor_GrantOutputDelegateVTable = nullptr;
    inline int32_t ABuildingItemCollectorActor_GrantOutputDelegateExecuteIndex = -1;
    // 9.00+: the output timer's delegate is a weak lambda (the collector its context), not a plain lambda.
    inline bool ABuildingItemCollectorActor_bGrantOutputDelegateIsWeakLambda = false;
    inline int32_t AFortGameModeZone_CreateAIDirectorIndex = -1;
    // AFortGameModeZone::CreateAIGoalManager's slot (intact; 6.20 CL-4497486 0xECF550, slot 0x181), the call
    // AFortGameModeZone::PostInitializeComponents makes right after CreateAIDirector's (Finder::ResolveAIDirectorLive).
    inline int32_t AFortGameModeZone_CreateAIGoalManagerIndex = -1;
    inline void (*AFortAIDirector_StopEncounter)(UObject* This, UObject* EncounterToStop, uint8_t EncounterStopReason, bool bForceKillAI, bool bBroadcastStop) = nullptr;
    inline void (*AFortWorldManager_ServerTickLoadingOrCreatingWorld)(AFortWorldManager* This) = nullptr;
    inline FFortMissionManagerRecord* (*FFortMissionManagerRecord_FFortMissionManagerRecord)(FFortMissionManagerRecord* This, UClass* InMissionManagerClass) = nullptr;
    inline void (*AFortGameState_SpawnMissionManager)(AFortGameState* This, const FFortMissionManagerRecord& MissionManagerRecord) = nullptr;
    // AFortGameState::OnMissionManagerCreatedEvent (21.40's name; an unreflected TMulticastDelegate<void()>), which
    // SpawnMissionManager broadcasts last, once the mission manager has loaded its record's missions (7.10 0xEEF970:
    // lea rcx, [rbx+678h]; call). Read from SpawnMissionManager on first use (AFortGameState::GetOnMissionManagerCreatedEvent):
    // -1 not read yet, 0 not there.
    inline ptrdiff_t AFortGameState_OnMissionManagerCreatedEvent = -1;
    inline void* (*UFortMissionInfo_GenerateMissionDifficultyOptions)(UFortMissionInfo* This, void* Result, void* RandomStream, const void* GenerationParams) = nullptr;
    inline ptrdiff_t AFortWorldManager_MissionGenerationRandomStream = -1;
    inline int32_t AFortWorldManager_ConditionalFoundationRequestSize = 0;
    inline ptrdiff_t AFortWorldManager_ConditionalFoundationRequestClasses = -1;
    inline ptrdiff_t AFortWorldManager_ConditionalFoundationRequestsArray = -1;
    inline void (*AFortWorldManager_ConditionalFoundationQuota)(AFortWorldManager* This) = nullptr;
    inline ptrdiff_t AFortWorldManager_ConditionalFoundationTierIndex = -1;
    inline int32_t AFortMission_ObjectiveStatusUpdatedIndex = -1;
    inline bool (*AFortMission_InitializeActorsForMission)(UObject* Context, UObject* Mission, void* OutActorsInitialized) = nullptr;
    inline int32_t AFortMission_CustomHandleMissionEventIndex = -1;
    inline int32_t AFortMission_HandleDeferredMissionEventIndex = -1;
    inline int32_t AFortMission_GrantBadgeIndex = -1;
    inline void* (*AFortWorldManager_GetTheaterMapData)(AFortWorldManager* This) = nullptr;
    inline int32_t AFortObjectiveBase_ActivateObjectiveIndex = -1;
    // Creates Object's missing UFortMissionEventParams objects (2.5 0xC720E0); see Finder::ResolveMissionEventParamsLive.
    // Null where the build inlines it into AFortMissionState::PostInitializeComponents (5.30 to 6.01), with the slot found.
    inline void (*InitializeMissionEventParams)(UObject* Object) = nullptr;
    inline int32_t AActor_PostInitializeComponentsIndex = -1;
    inline ptrdiff_t UFortGameData_MissionGenerationData = -1;
    inline bool (*UFortSchematicItem_CanCraftSchematic)(UFortSchematicItem* This, AFortPlayerController* PlayerController, uint8_t* OutFailCause, bool bConsiderReservedItems, int32_t Count) = nullptr;
    inline UFortSchematicItem* (*AFortPlayerController_GetSchematicForItem)(AFortPlayerController* This, const UFortItemDefinition* ItemDefinition) = nullptr;
    inline int32_t UFortSchematicItem_ServerCraftSchematicIndex = -1;
    inline bool (*AFortPlayerController_DisassembleInventoryItem)(AFortPlayerController* This, const FGuid& ItemGuid) = nullptr;
    // 1.10's batch check, over { FGuid ItemGuid; int32_t Count; } pairs (20 bytes each).
    inline bool (*AFortPlayerController_DisassembleInventoryItems)(AFortPlayerController* This, const FScriptArray& GuidsAndCounts) = nullptr;
    inline FScriptArray* (*UFortWorldItemDefinition_GetDisassemblyResults)(const UFortWorldItemDefinition* This, FScriptArray* OutResults, int32_t Unused, float NormalizedDurability) = nullptr;
    inline int32_t UFortWorldItem_SetLoadedAmmoIndex = -1;
    inline int32_t UFortWorldItem_SetNumInStackIndex = -1;
    inline int32_t UFortWorldItem_SetDurabilityIndex = -1;
    inline int32_t UFortNeverPersistItemDefinition_OnItemCountAddedIndex = -1;
    inline int32_t UFortNeverPersistItemDefinition_OnItemCountRemovedIndex = -1;
    inline int32_t AFortMissionManager_OnZoneEndIndex = -1;
    inline int32_t UFortMcpProfileWorld_BindToPlayerControllerIndex = -1;
    inline int32_t AFortGameSession_OnVerifyAuthCompleteIndex = -1;
    // Whether AFortGameSession::OnVerifyAuthComplete returns on a successful verify (to 2.3), or kicks either way (2.4).
    inline bool AFortGameSession_bOnVerifyAuthCompleteTestsSuccess = true;
    inline UObject* (*AFortAIGoalManager_FindAssignmentInList)(UObject* This, const void* Identifier, void* Assignments, int32_t Kind) = nullptr;
    inline uint64_t (*FEnvQueryInstance_AddItemData)(void* This, const FWeakObjectPtr& Item) = nullptr;
    inline ptrdiff_t FEnvQueryInstance_RawData = -1;
    inline ptrdiff_t FEnvQueryInstance_ValueSize = -1;
    inline ptrdiff_t FEnvQueryInstance_Owner = -1;
    inline ptrdiff_t UFortAIAssignment_Goals = -1;
    inline bool (*UFortAIAssignment_CanPawnUse)(UObject* This, APawn* Pawn) = nullptr;
    inline bool (*FFortAIGoal_CanAIUse)(const void* Goal, UObject* AI, UObject* Settings) = nullptr;
    inline void* (*AFortAIGoalManager_GetAssignmentInfo)(UObject* This, UObject* AI) = nullptr;
    // 5.0+ keeps AFortAIGoalManager::SetFrustrationDiscouragement(AFortAIController*, float) (6.00 CL-4395664 0xA93B00,
    // 5.41 0xA6C840, 5.30 0xA69490): the BT task's intact ExecuteTask calls it; only SetCurrentGoalDiscouragement's exec
    // is stripped. Null on 1.1-4.5, where Core restores both.
    inline bool (*AFortAIGoalManager_SetFrustrationDiscouragement)(UObject* This, UObject* AI, float DiscouragementDuration) = nullptr;
    inline ptrdiff_t FAIAssignmentInfo_DiscouragedGoalTimers = -1;
    // AFortAIGoalManager::GetCurrent's stripped body (return 0), and the calls to it AFortAIGoalManager::HookCallSites
    // redirected (they may still read as calls to the stub while a packed build holds code writes). A function that
    // calls neither lost its GetCurrent to the compiler, and everything after it (6.21 CL-4526925, 3.2).
    inline uintptr_t AFortAIGoalManager_GetCurrentStub = 0;
    inline std::vector<uint8_t*> AFortAIGoalManager_GetCurrentCalls;
    // AFortAIGoalManager::RegisterAIForGoalDetection / UnregisterAIForGoalDetection(AFortAIController*) (6.21 CL-4526925
    // 0xAF6C30 / 0xAFC520, 6.20 CL-4497486 0xACEAA0 / 0xAD7480): an AI's entry in the goal manager's assignments.
    inline void (*AFortAIGoalManager_RegisterAIForGoalDetection)(UObject* This, UObject* AI) = nullptr;
    inline void (*AFortAIGoalManager_UnregisterAIForGoalDetection)(UObject* This, UObject* AI) = nullptr;
    // What AFortAIGoalManager::OnReceiveGoalQueryResult does with a goal query's result when the AI's goal updates aren't
    // suppressed (6.21 0xAEEA70, 6.20 0xAC6E00): (AI, its FAIAssignmentInfo, const TSharedPtr<FEnvQueryResult>&).
    inline void (*AFortAIGoalManager_HandleGoalQueryResult)(UObject* This, UObject* AI, void* AssignmentInfo, const void* Result) = nullptr;
    // FAIAssignmentInfo's unreflected TSharedPtr<FEnvQueryResult> LastQueryResult (+0x58 on 3.2 to 6.21).
    inline ptrdiff_t FAIAssignmentInfo_LastQueryResult = -1;
    // UFortBTService_ContextOverride::ApplyValueOverrides / RevertValueOverrides, which its OnBecomeRelevant and
    // OnCeaseRelevant forward to (slots 0x56 / 0x57 on 3.2 to 6.21).
    inline int32_t UFortBTService_ContextOverride_ApplyValueOverridesIndex = -1;
    inline int32_t UFortBTService_ContextOverride_RevertValueOverridesIndex = -1;
    inline int32_t UBTTaskNode_ExecuteTaskIndex = -1;
    inline int32_t UEnvQueryGenerator_GenerateItemsIndex = -1;
    inline int32_t UEnvQueryContext_ProvideContextIndex = -1;
    // 6.21 (CL-4526925, 0x1068E10) adds bForceExecution, which it hands on to the slot activation ([vtable+0x680]); the
    // builds before it never read r9 there.
    inline void (*AFortQuickBars_SwapQuickBarFocus)(UObject* This, int32_t InQuickBar, int32_t SlotOverride, bool bForceExecution) = nullptr;
    inline int32_t (*AFortQuickBars_FocusSlot)(UObject* This, int32_t InQuickBar, int32_t Slot, bool bUpdatePreviousFocusedSlot) = nullptr;
    inline int32_t IFortSpottableActorInterface_CanBeSpottedByIndex = -1;
    inline void* (*UObject_GetInterfaceAddress)(UObject* This, UClass* InterfaceClass) = nullptr;
    // AFortAthenaMutator_Ashton's FortGameStateAthena_CanInteractWithGameObject (8.50 slot 0xFC):
    // char (Mutator, bool* bOutResult, IFortInteractInterface* Interactable, AFortPlayerController* Controller); 2 answers.
    inline int32_t AFortAthenaMutator_Ashton_CanInteractWithGameObjectIndex = -1;
    // AFortAthenaMutator's FortGameModeAthena_CheckWinConditionOnPlayerRemoval (21.40's name; 8.50/8.51 slot 0xD2,
    // Endgame's 0x105CBE0 on 8.51): char (Mutator, AFortPlayerControllerAthena*, APlayerState* Killer, int32 PlayersLeft
    // before the removal, APawn* Finisher, const UFortWeaponItemDefinition*, uint8 DeathCause); 2 answers. -1 where not found.
    inline int32_t AFortAthenaMutator_CheckWinConditionOnPlayerRemovalIndex = -1;
    inline int32_t IFortAIEncounterInfoOwnerInterface_GetEncounterInfoIndex = -1;
    inline void (*UFortQueryItemType_Goal_SetContextHelper)(void* ContextData, const FScriptArray& Goals) = nullptr;
    inline ptrdiff_t UNavigationSystem_NavBuildingLockFlags = -1;
    // From 6.20: UFortMovementComp_Character::ManagedTickIndex, the component's place in the list that ticks it
    // (AFortGameState::ManagedCharMovementComponents, or the AI mutator's DeimosMoveComponents), off the clear in
    // AFortGameState::RemoveManagedCharacterMovementComponent (Finder::ResolveManagedCharacterMovementLive).
    inline ptrdiff_t UFortMovementComp_Character_ManagedTickIndex = -1;
    inline void (*UNavigationSystem_RebuildAll)(UNavigationSystem* This, bool bIsLoadTime) = nullptr;
    inline void (*UNavigationSystem_ConditionalPopulateNavOctree)(UNavigationSystem* This) = nullptr;
    inline ptrdiff_t UNavigationSystem_bNavOctreeLock = -1;
    inline void (*UNavigationSystem_RequestRegistration)(UNavigationSystem* This, ANavigationData* NavData, bool bTriggerRegistrationProcessing) = nullptr;
    // UNavigationSystem::Build's two calls before RebuildAll (6.00 CL-4395664 0x2BC9AD7 "call [rax+308h]", 0x2BC9AE3 "call
    // [rax+2B0h]"): SpawnMissingNavigationData's slot, and ProcessRegistrationCandidates' slot (virtual from 4.2) or, on
    // older builds, the function itself.
    inline int32_t UNavigationSystem_SpawnMissingNavigationDataIndex = -1;
    inline int32_t UNavigationSystem_ProcessRegistrationCandidatesIndex = -1;
    inline void (*UNavigationSystem_ProcessRegistrationCandidates)(UNavigationSystem* This) = nullptr;
    // static UNavigationSystem::InitializeForWorld(UWorld*, FNavigationSystemRunMode): makes the world's navigation system
    // (CreateNavigationSystem) when it has none and its settings enable navigation, then runs its OnWorldInitDone.
    inline void (*UNavigationSystem_InitializeForWorld)(UWorld* World, uint8_t Mode) = nullptr;
    // 4.2 (UE 4.20's NavigationSystem module): FNavigationSystem::AddNavigationSystemToWorld(UWorld&, RunMode,
    // bInitializeForWorld) (4.2 0x1E9D8F0) in its place. When the world has no navigation system it makes one from the
    // world settings' NavigationSystemConfig (UNavigationSystemConfig::CreateAndConfigureNavigationSystem, then
    // UWorld::SetNavigationSystem), then runs the system's InitializeForWorld(World, Mode).
    inline void (*FNavigationSystem_AddNavigationSystemToWorld)(UWorld& WorldOwner, uint8_t RunMode, bool bInitializeForWorld) = nullptr;
    // From 6.00 it takes the config third, AddNavigationSystemToWorld(UWorld&, RunMode, UNavigationSystemConfig*
    // NavigationSystemConfig, bInitializeForWorld) (6.00 0x232AFD0): a null config is the world settings'
    // NavigationSystemConfigOverride, else their NavigationSystemConfig. The pointer above is then this one.
    inline bool FNavigationSystem_AddNavigationSystemToWorldTakesConfig = false;
    // Battle Royale's navmesh tile cache (UAthenaNavSystem), from FRecastNavMeshGenerator::AddGeneratedTiles: Detour's
    // dtNavMesh::addTile (dtStatus, 64-bit tile refs), ARecastNavMesh's RecastNavMeshImpl, FPImplRecastNavMesh's
    // DetourNavMesh and the generator's DestNavMesh, ActiveTiles and NumActiveTiles.
    inline uint32_t (*dtNavMesh_addTile)(void* This, uint8_t* Data, int32_t DataSize, int32_t Flags, uint64_t LastRef, uint64_t* Result) = nullptr;
    inline ptrdiff_t ARecastNavMesh_RecastNavMeshImpl = -1;
    inline ptrdiff_t FPImplRecastNavMesh_DetourNavMesh = -1;
    inline ptrdiff_t FRecastNavMeshGenerator_DestNavMesh = -1;
    inline ptrdiff_t FRecastNavMeshGenerator_ActiveTiles = -1;
    inline ptrdiff_t FRecastNavMeshGenerator_NumActiveTiles = -1;
    // MinCellZ is 5.0+'s seventh argument (the grid's bottom Z in cells); the six-argument 1.1-4.5 function never reads it.
    inline void (*UBuildingStructuralSupportSystem_InitializeGrid)(UBuildingStructuralSupportSystem* This, const FVector* GridCenter, int32_t TileCells, int32_t TilesX, int32_t TilesY, int32_t CellsZ, int32_t MinCellZ) = nullptr;
    inline ptrdiff_t UBuildingStructuralSupportSystem_GridSize = -1;
    inline ptrdiff_t UBuildingStructuralSupportSystem_GridOrigin = -1;
    inline ptrdiff_t AFortNavigationGraph_Cells = -1;
    inline ptrdiff_t AFortNavigationGraph_BlockLinks = -1;
    inline ptrdiff_t AFortNavigationGraph_DirtyBlocks = -1;
    inline ptrdiff_t AFortNavigationGraph_Flags = -1;
    inline ptrdiff_t AFortNavigationGraph_PathSearch = -1;
    inline void (*AFortNavigationGraph_UpdateDirtyBlocks)(AFortNavigationGraph* This) = nullptr;
    inline int32_t (*FNavGraphCells_GetCellIndex)(FNavGraphCells* This, const int32_t* Coords) = nullptr;
    inline void* (*FNavGraphPathSearch_Construct)(void* This, AFortNavigationGraph* Graph) = nullptr;
    // The game's terrain loader, which AFortNavigationGraph::OnWorldInitialized calls (6.02 CL-4461277 0xAD4620 from
    // 0xAD84C0): every loaded level's AFortWorldSettings::WorldCells into the graph's cells. Null where it isn't found.
    inline void (*AFortNavigationGraph_LoadTerrainCells)(UWorld* World, UBuildingStructuralSupportSystem* SupportSystem, AFortNavigationGraph* Graph) = nullptr;
    inline ptrdiff_t UFortMcpProfileWorld_Inventory = -1;
    inline ptrdiff_t UFortMcpProfileWorld_bIsInventoryInitialized = -1;
    // AFortPlayerController slots BindToPlayerController hands a rejoining player's existing inventory to (-1 when the
    // bind has no such branch).
    inline int32_t AFortPlayerController_ReconnectWorldInventoryIndex = -1;
    inline int32_t AFortPlayerController_ReconnectOutpostInventoryIndex = -1;
    inline int32_t AFortGameModeZone_OnZoneEndIndex = -1;
    inline int32_t AFortMissionManager_OnMissionStatusChangeIndex = -1;
    inline int32_t AFortGameStateZone_SetGameplayStateIndex = -1;
    inline void (*UFortQuestManager_SendStatEvent)(UFortQuestManager* This, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush) = nullptr;
    // SendStatEventWithTags(This, the custom stat name (16 bytes, empty for a typed event), Type, ..., Count, bFlush), and its
    // flush, which takes the completion delegate by value (0x38 bytes, all zero: unbound) and destroys it.
    inline void (*UFortQuestManager_SendStatEventWithTags)(UFortQuestManager* This, const void* CustomStatName, uint8_t Type, UObject* TargetObject, const FGameplayTagContainer& TargetTags, const FGameplayTagContainer& SourceTags, const FGameplayTagContainer& ContextTags, int32_t Count, bool bFlush) = nullptr;
    inline void (*UFortQuestManager_FlushStatEvents)(UFortQuestManager* This, void* OnComplete) = nullptr;
    // Builds that strip SendStatEventWithTags and SaveStatEvents (1.10): SendCustomStatEvent(This, the stat's
    // FDataTableRowHandle, Count, bForceFlush), which only validates the stat, and the calls the game makes to the shared
    // empty function in place of the typed wrapper (Finder::StrippedQuestStatEventCalls). Core sends the events itself.
    inline void (*UFortQuestManager_SendCustomStatEvent)(UFortQuestManager* This, const void* ObjectiveStat, int32_t Count, bool bForceFlush) = nullptr;
    inline bool UFortQuestManager_bStatEventsStripped = false;
    inline void (*UFortQuestManager_SendEndPrimaryMission)(UFortQuestManager* This, const FString& MatchmakingSessionId, const FString& MissionName, const FString& MissionStatus, FScriptArray& EarnedBadges, int32_t NumMissionPoints, bool bCriticalMatchBonus) = nullptr;
    inline void* (*UGameInstance_GetGameSessionId)(UGameInstance* This, void* Result) = nullptr;
    inline FString* (*UEnum_GetEnumName)(const UObject* This, FString* Result, int32_t Index) = nullptr;
    inline void (*AFortGameMode_SetBadgeScores)(AFortGameMode* This) = nullptr;
    inline int32_t AFortGameMode_SendEarnScoreIndex = -1;
    // 5.21's SendEarnScore (slot 0x140, 0xE07650) is these two steps: the players' scores (0xDF6220, an out
    // TArray<FAccountIdAndScore>) and the score grant (slot 0x13A, 0xE0E460: PlayersAndScores, bIsFinal). Its EarnScore
    // send is gone, as EndMatch carries each player's score.
    inline void (*AFortGameMode_GetPlayersAndScores)(AFortGameMode* This, FScriptArray* OutPlayersAndScores) = nullptr;
    inline int32_t AFortGameMode_GrantScoreRewardIndex = -1;
    inline void* (*AFortGameMode_MakeEarnScoreContext)(void* Result, void* Players) = nullptr;
    inline void (*TArray_FAccountIdAndScore_ResizeGrow)(FScriptArray* Array, int32_t OldNum) = nullptr;
    inline int32_t AFortPlayerState_GetStatIndex = -1;
    inline void (*AFortGameMode_SendStayWithParty)(AFortGameMode* This) = nullptr;
    inline int32_t AGameMode_ReturnToMainMenuHostIndex = -1;
    inline void* (*FDelegateBase_Allocate)(void* Delegate, int32_t Size) = nullptr;
    inline uint64_t (*FDelegateHandle_GenerateNewID)() = nullptr;
    inline void* McpRequestDelegateInstanceVTable = nullptr;
    inline ptrdiff_t FBaseUrlContext_Delegate = -1;
    inline ptrdiff_t FBaseUrlContext_UrlConfigName = -1;
    inline ptrdiff_t FBaseUrlContext_ContextCredentials = -1;
    // 4.2: the context's delegate is a heap FDelegateBase (Data, DelegateSize), whose Allocate the build inlines.
    inline bool FBaseUrlContext_bHeapDelegate = false;
    inline void* (*FDateTime_UtcNow)(int64_t* Result) = nullptr;
    inline bool (*AFortPlayerControllerOutpost_GetFabricatorProgress)(AFortPlayerController* This, int64_t* DisintegrationStartTime, int32_t* QuantumGooCompleted, int32_t* QuantumGooIncoming) = nullptr;
    inline void (*UFortRegisteredPlayerInfo_OnAccountInventoryChanged)(UFortRegisteredPlayerInfo* This, const void* ChangedItemTypes) = nullptr;
    inline int32_t AFortPlayerPawn_ResetGameplayAttributesIndex = -1;
    inline int32_t AFortGameMode_RedirectLootTierGroupIndex = -1;
    // From 1.11 (Finder::ResolvePlaylistDataTablesLive).
    inline void (*AFortGameModeAthena_ApplyPlaylistDataTables)(AFortGameModeAthena* This) = nullptr;
    // From 3.0 (Finder::ResolvePoiVolumeLive): the two calls that make a POI volume's body in
    // AFortPoiVolume::PostInitializeComponents (3.0 0x9FDF20): FInitBodySpawnParams' constructor (0x21F5570) and
    // FBodyInstance::InitBody (0x2206250).
    inline void* (*FInitBodySpawnParams_Construct)(void* This, const void* PrimComp) = nullptr;
    inline void (*FBodyInstance_InitBody)(void* This, void* Setup, const FTransform& Transform, void* PrimComp, void* InRBScene, const void* SpawnParams) = nullptr;
    // From 3.3 (Finder::ResolveSupplyDropSpawnLive): AFortAthenaMapInfo::PickSupplyDropLocation (3.3 0x9F8E80) and
    // SpawnSupplyDropAtLocation (0xA03800), which the Supply Llamas are placed with. From 3.6 SpawnSupplyDropAtLocation
    // also takes the heights its ground trace runs between (3.6 0xA08A90, 4.2 0xA28200: TraceStartZ, TraceEndZ, passed on
    // to AFortAthenaSupplyDrop::FindGroundLocationAt); before, FindGroundLocationAt read them from the map info. The two
    // extra arguments are ignored by the older four-argument function.
    inline FVector* (*AFortAthenaMapInfo_PickSupplyDropLocation)(AFortAthenaMapInfo* This, FVector* Result, const FVector& Center, float Radius) = nullptr;
    inline AFortAthenaSupplyDrop* (*AFortAthenaMapInfo_SpawnSupplyDropAtLocation)(AFortAthenaMapInfo* This, const FVector& Location, const FRotator& Rotation, UClass* SupplyDropClass, float TraceStartZ, float TraceEndZ) = nullptr;
    inline bool AFortAthenaMapInfo_SpawnSupplyDropAtLocationTakesHeights = false;
    // 4.2 (Finder::ResolveFlightPathLive): AFortAthenaMapInfo::IsMapInfoValidForAircraft (4.2 0xA19370) and
    // InitializeFlightPath(GameState) (0xA17EB0; 21.40's names), the aircraft paths' build, which 4.2 runs only from
    // AFortGameModeAthena::OnPlaylistDataLoaded (0xA1BE40). Null on 3.5 to 4.1, whose StartAircraftPhase builds them too.
    inline bool (*AFortAthenaMapInfo_IsMapInfoValidForAircraft)(AFortAthenaMapInfo* This) = nullptr;
    inline void (*AFortAthenaMapInfo_InitializeFlightPath)(AFortAthenaMapInfo* This, AFortGameStateAthena* GameState) = nullptr;
    // 3.5 to 4.5 (Finder::ResolveSetCurrentPlaylistIdLive): AFortGameStateAthena::SetCurrentPlaylistId (4.1 0xA6DD20,
    // 4.2 0xA592E0, 4.5 0xA7FF10), which gives the game state the game mode's playlist and loads its assets
    // (OnPlaylistDataLoaded follows). 4.1's AFortGameModeAthena::InitGameState (0xA2B880) calls it at the world's start;
    // 4.2 only from OnGivenMatchAssignment (0xA1B5F0). Null from 5.0, which inlines it there.
    inline void (*AFortGameStateAthena_SetCurrentPlaylistId)(AFortGameStateAthena* This, int32_t PlaylistId) = nullptr;
    // Finder::ResolveMatchAssignmentLive. AFortGameMode::OnGivenMatchAssignment(bSuccess, MatchOptions) (5.21 0xE0F360,
    // 5.30 0xE31710): the match options' custom key and playtest name; the matchmaking service's assignment reaches the
    // game mode through it (a delegate on its vtable slot). MatchOptions: the assignment's TMap<FString, FString> of
    // match.option.* values. AFortGameMode::SetCurrentPlaylistName (5.30 0xE40340): the game mode's CurrentPlaylistName
    // and CurrentPlaylistId from its playlist manager's playlist of that name. AFortGameState::
    // StartStreamingAdditionalPlaylistLevel (21.40's name; 5.21 0xE1FE60, 5.30 0xE42750): streams one of the playlist's
    // AdditionalLevels in.
    inline void (*AFortGameMode_OnGivenMatchAssignment)(AFortGameMode* This, bool bSuccess, const void* MatchOptions) = nullptr;
    inline int32_t AFortGameMode_OnGivenMatchAssignmentIndex = -1;
    inline void (*AFortGameMode_SetCurrentPlaylistName)(AFortGameMode* This, FName PlaylistName) = nullptr;
    inline void (*AFortGameState_StartStreamingAdditionalPlaylistLevel)(AFortGameState* This, FName LevelName) = nullptr;
    // From 6.01 (Finder::ResolveMatchAssignmentLive): AFortGameState::MatchOptions (21.40's name; unreflected, 6.01
    // +0x5F0), the TMap<FString, FString> AFortGameMode::OnGivenMatchAssignment leaves the assignment's match options in,
    // with the party's custom match options as pairs of their own (0xE4C2A0 splits match.option.customMatchOptions).
    // -1 where the game mode gives the game state none.
    inline ptrdiff_t AFortGameState_MatchOptions = -1;
    // From 6.01 (Finder::ResolveShouldDropItemLive): AFortPlayerController's virtual that says whether an item drops,
    // given what it would otherwise do (slot 0x31D; AFortPlayerControllerAthena's, 0xC26970, asks the game state's
    // mutators). -1 where the build has none.
    inline int32_t AFortPlayerController_ShouldDropItemIndex = -1;
    // Where Finder::ResolveOnPostEarnedKillLive finds it (6.31): the slot of AFortAthenaMutator's
    // FortGameStateAthena_OnPostEarnedKill (21.40's name; 6.31 CL-4573279 slot 0xD4, the empty function in AFortAthenaMutator,
    // AFortAthenaMutator_TDM's 0xD3C1B0), which 6.31 calls with the killer's controller. The game state's mutators hear
    // of a kill through it, and Team Rumble's scores it. Nothing in the client build calls it; the server's call was
    // stripped with its kill handling. -1 where not found.
    inline int32_t AFortAthenaMutator_OnPostEarnedKillIndex = -1;
    // With it: AFortTeamInfoAthena's CurrentTeamScore (21.40's name; unreflected, 6.31 +0x428), the team's score that the
    // TDM mutator adds its kills to and the game mode's Tick ranks the teams by. -1 where not found.
    inline ptrdiff_t AFortTeamInfoAthena_CurrentTeamScore = -1;
    // From 6.01 (Finder::ResolveIsRespawningAllowedLive): the slot of AFortGameModeAthena's own IsRespawningAllowed
    // (6.01 0xBCF370, 6.02 0xBD08A0, both slot 0x135), which the pawn's death asks before it has the player respawn.
    // -1 where it isn't found: the builds before 6.01, whose game state doesn't reflect bAllowRespawning.
    inline int32_t AFortGameModeAthena_IsRespawningAllowedIndex = -1;
    // Where Finder::ResolveDBNOEnabledLive finds it (6.31 0xC9B300): the game mode's DBNO check for a player, which the
    // player pawn's knock asks (AFortPlayerPawnAthena 0xD1C4A0) before it downs one. The game mode's mutators first, then
    // the playlist's DBNOType: no knock for NotWhenRespawning while the player would respawn (the game mode's
    // IsRespawningAllowed, slot 0x137), nor for Off. bDBNOEnabled without a game state.
    inline bool (*AFortGameModeAthena_GetDBNOEnabled)(AFortGameModeAthena* This, AFortPlayerState* PlayerState, AFortGameStateAthena* GameState) = nullptr;
    // From 5.21 (Finder::ResolveLootDataTablesLive): the slots of AFortGameMode::GetLootTierDataTables and
    // GetLootPackageDataTables (21.40's names; 6.02 slots 0x132 and 0x133), the loot tables every roll reads. The game
    // mode's own (6.02 0xE43050, 0xE43030) are the game data's LootTierDataTables and LootPackageDataTables;
    // AFortGameModeAthena's (6.02 0xBCB900, 0xBCB830) are the game state's playlist's LootTierData and LootPackages,
    // else its Super's. -1 where they aren't found: before 5.21 ApplyPlaylistDataTables copies the playlist's rows into
    // the default tables instead.
    inline int32_t AFortGameMode_GetLootTierDataTablesIndex = -1;
    inline int32_t AFortGameMode_GetLootPackageDataTablesIndex = -1;
    // Finder::ResolvePersistentGameplayEffectsLive. AFortGameStateZone::OnAbilitySystemInterfaceReadyForPersistentGameplayEffects
    // (21.40's name; 4.2 0xC3D5E0, 5.21 0xE5FC60, 5.41 0xE81110, 6.00 0xEC4B30): every active gameplay modifier's
    // persistent effects and ability sets (the playlist's ModifierList, a Save the World zone's modifiers) on an actor
    // whose ability system is ready, from the game state's global environment ability actor. Null where the build has no
    // stat name to find it by (1.x).
    inline void (*AFortGameStateZone_OnAbilitySystemInterfaceReadyForPersistentGameplayEffects)(AFortGameStateZone* This, IAbilitySystemInterface* AbilitySystemInterface) = nullptr;
    inline int32_t AFortPlayerController_GetOrInitializeHeroIndex = -1;
    inline int32_t AFortPlayerController_DecoSpawnedIndex = -1;
    // From 7.40 (Finder::ResolveCreateBuildingAndSpawnDecoLive): the call in AFortDecoTool::ServerCreateBuildingAndSpawnDeco_
    // Implementation (7.40 0x15C20D0, 8.00 0x15F9AE5) that should create the floor or wall a trap is placed on, a call into
    // the shared return-zero stub. Null on builds without the RPC.
    inline uint8_t* AFortDecoTool_ServerCreateBuildingAndSpawnDeco_CreateBuildingCall = nullptr;
    // The same call in AFortDecoTool_ContextTrap's override (8.00 0x15F9D4E, 8.20 0x170117C), which the Chiller and Damage Trap use.
    inline uint8_t* AFortDecoTool_ContextTrap_ServerCreateBuildingAndSpawnDeco_CreateBuildingCall = nullptr;
    // From 8.00 (Finder::ResolveOctopusTowhookLive): the Baller's grapple. The call in AFortOctopusTowhookAttachableProjectile::
    // OnRep_ReplicatedAttachedInfo (8.00 0x161C3BD, 8.20 CL-5625478 0x1723E7C) that hands the hit to the owning
    // AFortOctopusVehicle, a call into the shared empty function. UPrimitiveComponent::GetBodyInstance's slot and
    // FBodyInstance::GetUnrealWorldTransform (8.20 0x2BAC2F0, 8.00 0x2B06090), which FTowhookAttachState's world getter
    // (8.20 0xF57770, 8.00 0xE60900) places the attach point with.
    inline uint8_t* AFortOctopusTowhookAttachableProjectile_OnRep_ReplicatedAttachedInfo_SetTowhookAttachStateCall = nullptr;
    inline int32_t UPrimitiveComponent_GetBodyInstanceIndex = -1;
    inline FTransform* (*FBodyInstance_GetUnrealWorldTransform)(const void* This, FTransform* Result, bool bWithProjection, bool bForceGlobalPose) = nullptr;
    inline int32_t AFortGameMode_ScoreBuildingConstructionIndex = -1;
    // From 3.0 (Finder::ResolveGetPoiTagsForLive): AFortGameMode::GetPoiTagsFor, a pawn's location tags (slot 0x144 on
    // 3.6 and 4.0; AFortGameModeAthena's asks its POI manager, AFortGameModeZone's is the shared empty function).
    inline int32_t AFortGameMode_GetPoiTagsForIndex = -1;
    // The native helpers of the building RPCs 1.8.1 strips (Finder::ResolveBuildingRestorationLive).
    inline int32_t AFortGameMode_ScoreBuildingRepairIndex = -1;
    inline int32_t AFortGameMode_ScoreBuildingEditIndex = -1;
    inline int32_t AFortPlayerController_CanPerformActionIndex = -1;
    inline int32_t AFortPlayerController_HasPermissionToEditWorldIndex = -1;
    inline int32_t AFortPlayerController_CanAffordToPlaceBuildableClassIndex = -1;
    inline int32_t AFortPlayerController_CanAffordToRepairIndex = -1;
    inline int32_t AFortPlayerController_IsBuildingRestrictedIndex = -1;
    inline int32_t AFortPlayerController_HasRequiredBuildingLevelIndex = -1;
    inline int32_t AFortPlayerController_CanPlaceBuildableClassInStructuralGridIndex = -1;
    inline int32_t AFortPlayerController_PayBuildableClassPlacementCostIndex = -1;
    inline int32_t AFortPlayerController_PayBuildingRepairCostIndex = -1;
    inline int32_t ABuildingActor_DieIndex = -1;
    inline int32_t ABuildingActor_InitializeBuildingActorIndex = -1;
    inline int32_t ABuildingActor_PostInitializeSpawnedBuildingActorIndex = -1;
    inline int32_t ABuildingSMActor_SetMirroredIndex = -1;
    inline int32_t ABuildingSMActor_OnServerAttemptBuildingUpgradeIndex = -1;
    inline int32_t ABuildingSMActor_IsBeingPlayerEditedIndex = -1;
    inline int32_t ABuildingSMActor_ReplaceBuildingActorIndex = -1;
    inline int32_t ABuildingSMActor_CanBePlayerEditedIndex = -1;
    inline int32_t ABuildingSMActor_GetEditingPlayerIndex = -1;
    inline int32_t ABuildingSMActor_SetEditingPlayerIndex = -1;
    // 9.00 (CL-6337466): the intact code AFortCreativeMoveTool's restored server functions call (Finder::
    // ResolveCreativeMoveToolLive). Null or -1 where the build doesn't have that shape.
    inline uintptr_t AFortCreativeMoveTool_DetermineActorMoveMode = 0;
    inline uintptr_t AFortCreativeMoveTool_GetInteractionBehaviorForActor = 0;
    inline uintptr_t AFortCreativeMoveTool_IsOverlappingIdenticalActor = 0;
    inline uintptr_t FFortCreativeBudgetTracker_CanAddActors = 0;
    // The global table of asset costs FFortCreativeBudgetTracker reads (TMap<FCreativeActorId, FCreativeAssetCost>).
    inline void* FortCreativeAssetCosts = nullptr;
    inline uintptr_t LevelSave_CreateTemplate = 0;
    inline uintptr_t LevelSave_ApplyTemplate = 0;
    inline uintptr_t KismetSystemLibrary_ConfigureCollisionParams = 0;
    inline uintptr_t UWorld_LineTraceSingleByChannel = 0;
    inline uintptr_t UWorld_LineTraceMultiByChannel = 0;
    inline uintptr_t ABuildingSMActor_ApplySavedSupportStatus = 0;
    inline uintptr_t ABuildingSMActor_AddToStructuralGrid = 0;
    // What the start of a move does to a building (9.00 0x11B7160: stops its build animation, among others).
    inline uintptr_t ABuildingSMActor_PrepareForMove = 0;
    // The static TMulticastDelegate<void(AActor*)> a building's move start and end broadcast (relevancy).
    inline void* BuildingActorRelevancyChanged = nullptr;
    inline int32_t ABuildingActor_ResolveCollisionTouchesIndex = -1;
    inline int32_t AActor_GetComponentsBoundingBoxIndex = -1;
    inline int32_t AActor_CalculateComponentsBoundingBoxInLocalSpaceIndex = -1;
    inline int32_t ABuildingSMActor_NeedsRepairIndex = -1;
    inline int32_t ABuildingSMActor_RepairBuildingIndex = -1;
    inline int32_t UCharacterMovementComponent_ServerMove_ImplementationIndex = -1;
    inline int32_t AFortPlayerPawn_EquipLastWeaponIndex = -1;
    inline int32_t AFortPawn_EquipDefaultWeaponIndex = -1;
    // UFortGameplayAbility::AbilityGrantedWeapon (21.40: the TWeakObjectPtr<AFortWeapon> after AbilityGrantedWeaponItem),
    // where the build's EndAbility cleanup reads it to give the ability's weapon its lifespan (7.00 0x9ED226, 7.10
    // 0x9FEC16: lea rsi, [rdi+8F0h] ... SetLifeSpan(3.0)); -1 where the cleanup has no such step.
    inline ptrdiff_t UFortGameplayAbility_AbilityGrantedWeapon = -1;
    // UFortGameplayAbility::GetActivatingPawn, the C++ function its exec thunk calls (6.31 0xA88B20, 7.00 0x9EDB60, 7.10
    // 0x9FF550, 7.20 0xAD6CD0: Cast<AFortPawn>(CurrentActorInfo->AvatarActor)). Called directly, not through ProcessEvent,
    // which rejects an ability that its EndAbility has already marked pending kill; null where the thunk doesn't call it.
    inline AFortPawn* (*UFortGameplayAbility_GetActivatingPawn)(const UGameplayAbility* This) = nullptr;
    // The game code a DBNO revive runs (1.8 AFortPlayerPawn::ServerReviveFromDBNO_Implementation, 0x7FF160), for the
    // builds that strip that implementation (1.8.1): Finder::ResolveDBNOReviveLive.
    inline int32_t AFortPlayerPawn_CanReviveFromDBNOIndex = -1;
    inline int32_t AFortPawn_StopNativeActionIndex = -1;
    inline void (*UAbilitySystemComponent_CancelAbilities)(UAbilitySystemComponent* This, const FGameplayTagContainer* WithTags, const FGameplayTagContainer* WithoutTags, UGameplayAbility* Ignore) = nullptr;
    inline void (*AFortPlayerPawn_UpdateDBNOInteractCollision)(AFortPawn* This) = nullptr;
    // AFortPlayerPawn::TryToAutoPickupWeapon (7.40 0x13DF6D0, 8.00 0x141E6E0) and the local player controller lookup it
    // opens with (7.40 0x10A9FA0, called (World, true, true)), where the game's tick reaches it: Finder::ResolveAutoPickupLive.
    inline void (*AFortPlayerPawn_TryToAutoPickupWeapon)(AFortPlayerPawn* This, AFortPickup* Pickup) = nullptr;
    inline APlayerController* (*AFortPlayerPawn_AutoPickupLocalPlayerController)(UWorld* World, bool bArg1, bool bArg2) = nullptr;
    inline ptrdiff_t AFortPlayerPawn_DBNOTimerHandle = -1;
    inline void (*AFortPawn_ForgetPawnForAllAI)(AFortPawn* This) = nullptr;
    inline int32_t AFortPlayerControllerZone_OnPawnRevivedIndex = -1;
    inline int32_t AFortPlayerControllerZone_ApplyResurrectDurabilityPenaltyIndex = -1;
    // 6.21 (CL-4526925) adds the bool (true: then back to the primary bar); the builds before it never read dl there.
    inline void (*AFortPlayerController_RefocusQuickBars)(AFortPlayerController* This, bool bSwapToPrimaryQuickBar) = nullptr;
    // Set only where StartAircraftPhase's call to it is compiled in (2.2 0x85DCA0), see Finder::AircraftPhasePlayerCleanupCall.
    // 7.20 (CL-4727874 0x137B7F0): AFortPlayerPawn::OnRep_ZiplineState, a native function there (the pawn's zipline
    // movement and tags from ZiplineState).
    inline void (*AFortPlayerPawn_OnRep_ZiplineState)(UObject* This) = nullptr;
    // 7.20 (CL-4727874 0x1397820) adds bForceExecution, which it passes to ServerActivateSlotInternal ([rsp+30h]); builds
    // before it take five and ignore it.
    inline void (*AFortPlayerController_ActivateSlot)(AFortPlayerController* This, uint8_t InQuickBar, int32_t Slot, float ActivateDelay, bool bUpdatePreviousFocusedSlot, bool bForceExecution) = nullptr;
    inline void* (*UStatManager_FindStat)(UObject* This, FName StatName, uint8_t Type) = nullptr;
    inline void (*FStat_Increment)(void* Stat, int32_t Amount, int32_t Flags) = nullptr;
    // 1.10's stat change (0x904520) takes its amount as a double, in xmm1 (Finder::ResolveDBNOReviveLive sees the movsd
    // that loads it); 1.8's takes an int32.
    inline bool FStat_bDoubleAmount = false;
    // UStatManager::GetStatValue (1.10 0x9006E0): a stat's value in a recording period, rounded; Finder::ResolveMatchStatsLive.
    inline int32_t (*UStatManager_GetStatValue)(UObject* This, FName StatName, uint8_t Period) = nullptr;
    inline int32_t AFortPlayerControllerAthena_ServerDBNOReviveInterruptedIndex = -1;
    // What 1.9's AFortPlayerControllerAthena pawn-death handler (OnPawnDied, 0x42DB20) calls, for the builds that strip
    // it to a jump to AFortPlayerControllerZone's (1.9.1): Finder::StrippedAthenaPawnDiedIndex and
    // Finder::ResolveAthenaPawnDiedLive. The outlived count's pieces are 1.9.1's own (its wins').
    inline void (*AFortPlayerControllerZone_OnPawnDied)(AFortPlayerControllerAthena* This, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer) = nullptr;
    // What 3.5's stripped handler (3.5.1 0xA27A00) keeps after Zone's, on the server: the player's map marker removed
    // (3.5.1 0xA1B160: ServerRemoveMapCursor, then the player state's MapIndicatorPos zeroed). Null where the slot is a
    // jump to Zone's.
    inline void (*AFortPlayerControllerAthena_RemoveMapCursor)(AFortPlayerControllerAthena* This) = nullptr;
    // 7.00 (CL-4629139) makes the same two steps in the stripped handler itself (0xC5C980): the ServerRemoveMapCursor RPC's
    // thunk (0x1618840), then, for an AFortPlayerStateAthena, its MapIndicatorPos set to FVector2D::ZeroVector (0xC99610,
    // the vector by value in rdx). Null elsewhere.
    inline void (*AFortPlayerControllerAthena_ServerRemoveMapCursor)(AFortPlayerControllerAthena* This) = nullptr;
    inline void (*AFortPlayerStateAthena_SetMapIndicatorPos)(AFortPlayerStateAthena* This, FVector2D InMapIndicatorPos) = nullptr;
    // 8.00 (CL-5203069) moves the map marker to the controller's MarkerComponent: the stripped handler (0xDBDEF0) keeps,
    // on the server, one call of it with the component (0xE2FB30: the owner's map marker in MarkerStream goes through
    // ServerRemoveMapMarker). 21.40's UAthenaMarkerComponent::LocalRemoveMapMarker. Null elsewhere.
    inline void (*UAthenaMarkerComponent_LocalRemoveMapMarker)(UAthenaMarkerComponent* This) = nullptr;
    // 8.40+: the end of AFortPlayerControllerAthena's pawn-death handler (0xF88F12): AFortGameStateZone::
    // IsRespawningAllowed (slot 0x124), then the controller's respawn invalidation timer (0xF97C10). -1 / null before.
    inline int32_t AFortGameStateZone_IsRespawningAllowedIndex = -1;
    inline void (*AFortPlayerControllerAthena_StartRespawnInvalidationTimer)(AFortPlayerControllerAthena* This) = nullptr;
    // 9.10+: the unreflected flag (after bLeaveDisconnectedPawnsInGame) of a controller that stands in for a disconnected
    // player; the pawn-death handler destroys such a controller last.
    inline ptrdiff_t AFortPlayerControllerAthena_DestroyOnPawnDiedOffset = -1;
    // 9.00 (CL-6337466): the handler takes the dead pawn first (21.40's OnPawnDied(AFortPlayerPawn*, float, ...); the
    // pawn's calls, 0x1771477 and 0x10A8DF3, pass it in rdx and Damage in xmm2), and Zone's the same. Its stripped body
    // (0x1085440) first clears a timer of the pawn's (an unreflected FTimerHandle, +0xC40 in 9.00, that 0x176CEC0 sets
    // for the next tick). Null / -1 before 9.00.
    inline void (*AFortPlayerControllerZone_OnPawnDiedWithPawn)(AFortPlayerControllerAthena* This, AFortPlayerPawn* DiedPawn, float Damage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* Instigator, AActor* DamageCauser, AController* Downer) = nullptr;
    inline ptrdiff_t AFortPlayerPawn_PawnDiedTimerHandleOffset = -1;
    // AFortPlayerControllerZone's death report (1.10 0x8DB450), which AFortPlayerControllerAthena's override calls first
    // where its Athena part is stripped (1.9.1, 1.10): Finder::StrippedAthenaPrepareDeathReportIndex.
    inline void (*AFortPlayerControllerZone_PrepareDeathReport)(AFortPlayerControllerAthena* This, float LethalDamage, const FGameplayTagContainer& DamageTags, const FVector& Momentum, AController* KillerController, AActor* DamageCauser, bool bDroppedBackpack) = nullptr;
    // From 4.4 it also takes the finisher's pawn, weapon and the death cause, which it hands to the win checks and on to
    // ClientNotifyWon (4.5 0xA7B630; Logout's call, 0xA747C1, passes null, null, Unspecified). Earlier builds ignore them.
    // 6.00, 6.01, 6.02 and 6.10 (6.00 0xC307B0, 6.02 0xBDAD80; earlier builds not looked at) take one more, true only
    // from the game mode's PreTeamChange (a Playground team switch, 6.00 0xC2C980, death cause TeamSwitchSuicide): it
    // skips the win check at the end (the mutators' or CheckWinConditionLastManStanding, 0xC155A0). Logout (0xC27408)
    // and PawnPendingDestroy (0xC6596B) pass false. Left out, the callee read whatever the caller's stack held there, so
    // an elimination could skip the game's win check; a build without it ignores it.
    inline void (*AFortGameModeAthena_RemoveFromAlivePlayers)(AFortGameModeAthena* This, AFortPlayerControllerAthena* Player, APlayerState* KillerPlayerState, APawn* FinisherPawn, const UFortWeaponItemDefinition* FinishingWeapon, uint8_t DeathCause, bool bTeamChange) = nullptr;
    // AFortGameModeAthena::AddToAlivePlayers(AFortPlayerControllerAthena*) (8.30 0xF00870): the player into AlivePlayers,
    // their team's alive count, PlayersLeft and TeamsLeft, and bMarkedAlive. Found on first use (the Reboot Van's).
    inline void (*AFortGameModeAthena_AddToAlivePlayers)(AFortGameModeAthena* This, AFortPlayerControllerAthena* Player) = nullptr;
    // FGameMemberInfo::PostReplicatedAdd(const FGameMemberInfoArray&) with AFortGameStateAthena::HandleGameMemberAdded
    // inlined (8.20 0xEAE8D0, 8.30 0xF536C0): the item's player state into the game state's native team and squad maps.
    inline void (*FGameMemberInfo_PostReplicatedAdd)(void* Item, void* InArraySerializer) = nullptr;
    inline void (*AFortPlayerControllerAthena_SetTeamPlace)(AFortPlayerControllerAthena* This, int32_t Place) = nullptr;
    inline int32_t (*AFortPlayerStateAthena_GetSecondsAlive)(AFortPlayerStateAthena* This) = nullptr;
    // The death cause up to 3.3 (1.9.1 0x43AE20), on the pawn.
    inline uint8_t (*AFortPlayerPawn_GetDeathCause)(AFortPawn* This, const FGameplayTagContainer& DamageTags, bool bWasDBNO) = nullptr;
    // From 3.5 it is 21.40's static AFortPlayerStateAthena::ToDeathCause (3.5.1 0xA754E0, 3.6 0xA835B0, 4.0 0xAA41A0,
    // 4.1 0xAB04B0): the damage tags and whether the pawn was DBNO, no pawn. Only one of the two is set.
    inline uint8_t (*AFortPlayerStateAthena_ToDeathCause)(const FGameplayTagContainer& InTags, bool bWasDBNO) = nullptr;
    // From 6.01 (Finder::ResolveSetDeathInfoLive): the player state's own store of a death (6.02 CL-4461277 0xC4A090,
    // 6.01 CL-4424678 0xC478B0; the 21.40 exports don't name it, the name is Core's). It copies the FDeathInfo it is
    // given over the player state's DeathInfo, sets bInitialized, writes the kill feed line of a recorded replay and of
    // the spectator broadcast when one is on, and nothing else. The pawn's DBNO handling is its one caller left (6.02
    // 0xC5BF7C); the controller's pawn-death handler, which was the other, is stripped. Null where it isn't found.
    inline void (*AFortPlayerStateAthena_SetDeathInfo)(AFortPlayerStateAthena* This, const FDeathInfo& InDeathInfo) = nullptr;
    // From 6.01, found with it: what FDeathInfo::DeathCircumstance holds for a pawn (6.02 0xC5F320, 6.01 0xC5CAB0; not
    // named in the 21.40 exports either), a static that takes the pawn and may be given none. Bit 0 is a tag of the
    // pawn's own, bits 1 and 2 tags of the vehicle it was last in (VehicleStateLastTick): OnRep_DeathInfo's kill feed
    // reads bit 1 for a fall ("needs a cleanup in aisle 3", a shopping cart's). Null where it isn't found.
    inline int32_t (*AFortPlayerPawnAthena_GetDeathCircumstance)(const AFortPlayerPawnAthena* Pawn) = nullptr;
    // Score Royale's scoring of a tagged event, on the player state (5.30 0xC4B5A0, 5.40 0xC5A4F0): Finder::
    // ResolveScoringEventLive, AFortPlayerStateAthena::HandleScoringEvent.
    inline void (*AFortPlayerStateAthena_HandleScoringEvent)(AFortPlayerStateAthena* This, const FGameplayTagContainer& EventTags) = nullptr;
    inline int32_t AFortPlayerControllerAthena_UpdateTeamMembersIndex = -1;
    inline int32_t (*AFortPlayerControllerAthena_CountTeammatesOut)(AFortPlayerControllerAthena* This) = nullptr;
    inline ptrdiff_t AFortGameStateAthena_StartingPlayers = -1;
    inline ptrdiff_t AFortPlayerStateAthena_PlayersOutlived = -1;
    // AFortGameModeAthena::StartEndGamePhase (3.0 0x9C52C0, 21.40's name): the solo win, RemoveFromAlivePlayers' last call
    // ("%s won the match!", place 1 for the winner, then the end game); with no winner it only ends the match. Battle
    // Royale's player bots end a match with it (FCoreAthenaBotManager::CheckWinConditionLastManStanding).
    // From 4.4 ClientNotifyWon has parameters and the win passes its own on to it (4.5 0xA82160 -> 0x125B8E0): called with
    // the winner alone, they were whatever r8, r9 and the stack held, and the RPC's FinisherPawn faulted (AV reading 0xD).
    // Earlier builds ignore them.
    inline void (*AFortGameModeAthena_StartEndGamePhase)(AFortGameModeAthena* This, AFortPlayerControllerAthena* Winner, APawn* FinisherPawn, const UFortWeaponItemDefinition* FinishingWeapon, uint8_t DeathCause) = nullptr;
    // AFortGameModeAthena::StartAircraftPhase (3.0 0x9C4A60): Finder::AircraftPhasePlayerCleanupCall's function. Only
    // the bots-only test match (-CoreBotMatch) calls it, the warmup's countdown having nobody to start the bus for.
    // 7.20's (CL-4727874, 0xD1BEC0) takes a bool: set, the phase starts without spawning the aircraft (its warmup-skip
    // caller 0xD0C3E0 passes true, then starts the safe zones; the warmup's own 0xD09DD3 and the STARTAIRCRAFT command
    // 0xD03255 pass false). Called with This alone it took whatever dl held, and on Playlist_Deimos_Solo_Winter the bus
    // never spawned ("PlacePlayersInAircraft early out. Aircrafts TArray is empty", CoreV2 #490). Builds whose
    // StartAircraftPhase has no such parameter ignore it.
    inline bool (*AFortGameModeAthena_StartAircraftPhase)(AFortGameModeAthena* This, bool bSkipAircraft) = nullptr;
    // AFortPlayerPawnAthena::BeginSkydiving(bool bFromBus) (3.0 0x9D7F70), which AFortPlayerControllerAthena's ExitAircraft
    // calls on the new pawn: Finder::ResolveBotSkydiveLive. A player bot leaving the aircraft takes the same path. 1.7.2's
    // and 1.8's (1.8 0x41DD60) take no bFromBus yet; the extra argument in rdx goes unread there.
    inline void (*AFortPlayerPawnAthena_BeginSkydiving)(AFortPlayerPawnAthena* This, bool bFromBus) = nullptr;
    // 1.7.2 to 1.8.2 (CoreV2 #449): BeginSkydiving's server part (Role == ROLE_Authority) takes the pawn's controller
    // as an AFortPlayerControllerAthena with no null check, so an AI controller's pawn crashes it (1.8.2 0x4152FC:
    // "xor ebx, ebx; mov rcx, rbx; call" into the world context lookup 0x420260, which reads [rcx]). 1.9 (0x4171E8) only
    // runs that part from the bus and for a player controller. The offset of the flag the part sets first (1.8.2
    // +0x848), which 1.9 still sets for any controller; -1 where the build's BeginSkydiving has the guard.
    inline ptrdiff_t AFortPlayerPawnAthena_BeginSkydiving_UnguardedServerFlagOffset = -1;
    // AFortPawn::SetCurrentMovementStyle(const TEnumAsByte<EFortMovementStyle>&)'s slot (21.40's name; 5.10 0x1107A30,
    // slot 0x8C8): it sets CurrentMovementStyle and then the character movement's speeds for that style (MaxWalkSpeed and
    // the rest, from GetMovementSpeed). A bot's sprint goes through it: Finder::ResolveMovementStyleLive.
    inline int32_t AFortPawn_SetCurrentMovementStyleIndex = -1;
    // 8.30 (CL-5793395, 0x15CD970) takes the style by value (mov [rcx+6E0h], dl), earlier builds by reference.
    inline bool bAFortPawn_SetCurrentMovementStyleByValue = false;
    // From 6.x, the AI pawns that animate through the animation sharing system (Finder::ResolveSharedAnimLive): what the
    // build keeps of UFortAbilitySystemComponent::PlaySharedAnim (6.20 CL-4497486 0xA70D50: RepSharedAnimInfo = SharedAnim,
    // return 0; the ability and its activation info go unused), its calls in the abilities' animation tasks, and
    // AFortPawn::TriggerAbilityFromAnimation's slot (6.20 0x1241770, slot 0x171).
    inline float (*UFortAbilitySystemComponent_PlaySharedAnim)(UFortAbilitySystemComponent* This, UGameplayAbility* AnimatingAbility, const FGameplayAbilityActivationInfo* ActivationInfo, const FGameplayAbilityRepSharedAnim* SharedAnim) = nullptr;
    inline std::vector<uint8_t*> UFortAbilitySystemComponent_PlaySharedAnimCalls;
    inline int32_t AFortPawn_TriggerAbilityFromAnimationIndex = -1;
    // From 6.x: UAthenaAIPerceptionManager::OnAISettingsUpdated (21.40's name; 6.20 CL-4497486 0xA9DEA0), the footstep
    // hearing settings from a UAthenaAISettings, which UAthenaAISystem::StartPlay (0xAABEA0) hands it:
    // Finder::ResolveAthenaAIPerceptionLive.
    inline void (*UAthenaAIPerceptionManager_OnAISettingsUpdated)(UAthenaAIPerceptionManager* This, const UObject* Settings) = nullptr;
    // From 6.x: UAthenaAISystem::UnregisterManagedController (21.40's name), as AAthenaAIController's UnPossess calls it
    // (6.21 CL-4526925 0xAA6D80, 6.20 CL-4497486 0xAADC60, 6.22 0xAA8920): the controller only, whose world's AI system
    // it takes the controller (and its behavior tree and path following components) out of, each where its index isn't
    // -1. Finder::ResolveAthenaAIControllerUnregisterLive.
    inline void (*UAthenaAISystem_UnregisterManagedController)(AAthenaAIController* Controller) = nullptr;
    // From 6.x: AAthenaAIController::ManagedTickIndex (21.40's name), the controller's place in its AI system's
    // AIControllers, -1 while it is out: the unregister's first `mov dword ptr [reg+X], -1` (6.21 CL-4526925 +0x558 at
    // 0xAA6E0B, 7.20 CL-4727874 +0x570 at 0xB1C79C). The same finder.
    inline ptrdiff_t AAthenaAIController_ManagedTickIndex = -1;
    // FCoreUObjectDelegates::GetPreGarbageCollectDelegate()'s static FSimpleMulticastDelegate, which CollectGarbageInternal
    // broadcasts before it marks what is reachable (6.21 CL-4526925 0x51DE780 from the getter 0x196A2A0 called at
    // 0x18EB920; 7.20 CL-4727874 0x55FCCF0 from 0x1AE9CD0 at 0x1A73F5A; 7.30 CL-4834550 0x565A430; 7.40 CL-5046157
    // 0x56D0320): Finder::ResolvePreGarbageCollectLive.
    inline void* FCoreUObjectDelegates_PreGarbageCollect = nullptr;
    // FCoreDelegates::OnFEngineLoopInitComplete, the static FSimpleMulticastDelegate FEngineLoop::Init broadcasts last
    // (8.30 CL-5793395 0x54B2FD8, broadcast at 0x30ECE5; 8.40 CL-6005771 0x54F6058 at 0x311630):
    // Finder::ResolveEngineLoopInitComplete.
    inline void* FCoreDelegates_OnFEngineLoopInitComplete = nullptr;
    // AFortGameMode::AttemptChangeTeamInfoForController(uint8 Team, AFortPlayerController* Controller)'s slot (3.2 0xA88,
    // 0xB526C0; 21.40's name and signature): PlacePlayerOnTeam's change of a joining player's team, which puts the
    // controller in the team's AFortTeamInfo and its player state on the team. It takes any controller whose player
    // state is an AFortPlayerState, so Battle Royale's player bots join their team with it: Finder::ResolveTeamChangeLive.
    inline int32_t AFortGameMode_AttemptChangeTeamInfoForControllerIndex = -1;
    // AFortTeamInfo::RemoveFromTeam(AController* Controller) (4.2 0xCEDDC0, 21.40's name): takes the controller out of
    // TeamMembers, clears its player state's team (SetPlayerTeam(nullptr)) and has the team's AFortTeamPrivateInfo drop
    // the player's entries (4.2 0xFB3750), which the squad bar and the teammate markers read. A controller that isn't in
    // TeamMembers changes nothing. A player bot leaving the match leaves its team with it: Finder::ResolveTeamRemoveLive.
    inline void (*AFortTeamInfo_RemoveFromTeam)(AFortTeamInfo* This, AController* Controller) = nullptr;
    // UOnlineEngineInterface::CreateUniquePlayerId(const FString& Str) (3.1 UOnlineEngineInterfaceImpl's 0x316930, slot
    // 0x268; 2.5 and 3.0 the same slot): the identity interface's CreateUniquePlayerId, as a connection's id is made when
    // it replicates. UE 4.20 (3.3 0x328DE0, slot 0x278) adds FName Type: NAME_None is the default online subsystem's.
    // 6.20 (0x432F00, the same slot) calls the identity's CreateUniquePlayerId at +0x100 instead of +0xD0.
    // Battle Royale's player bots get their account-like ids from it: Finder::ResolveCreateUniquePlayerIdLive.
    inline int32_t UOnlineEngineInterface_CreateUniquePlayerIdIndex = -1;
    // AGameSession::GetNextPlayerID's static NextPlayerID (UE 4.20: "static int32 NextPlayerID = 256; return
    // NextPlayerID++;"), inlined into AGameSession::RegisterPlayer (3.1 0x2056E60, slot 0x608; 3.3 0x20DC8E0 and 3.5.1
    // 0x217C620, slot 0x610), which gives a joining player's state its PlayerId from it. Battle Royale's player bots
    // take theirs from it too: Finder::ResolveNextPlayerIDLive.
    inline int32_t* AGameSession_NextPlayerID = nullptr;
    // AGameSession::KickPlayer(APlayerController* KickedPlayer, const FText& KickReason)'s slot, as the game mode's kick of
    // a player who didn't load calls it (7.20 0xD0D76B, slot 0xCD): Finder::ResolveGameSessionKickLive.
    inline int32_t AGameSession_KickPlayerIndex = -1;
    // Finder::ResolveSessionJoinabilityLive, for Creative's friends' joins (Core-Backend #411):
    // AGameSession::GetSessionJoinability(FName, FJoinabilitySettings&) and UpdateSessionJoinability(FName, bool x4), the
    // slots after it (8.20 0x6A8/0x6B0, 0x29D5600 and AFortGameSession's 0x13B5A00); Online::GetSessionInterface(const
    // UWorld*, FName) (8.20 0x645130) and IOnlineSession::GetSessionSettings(FName)'s slot (0x2D8), as
    // UOnlineEngineInterfaceImpl::UpdateSessionJoinability (0x6665B0) calls them.
    inline int32_t AGameSession_GetSessionJoinabilityIndex = -1;
    inline int32_t AGameSession_UpdateSessionJoinabilityIndex = -1;
    inline TSharedPtr<IOnlineSession, ESPMode::ThreadSafe>* (*Online_GetSessionInterface)(TSharedPtr<IOnlineSession, ESPMode::ThreadSafe>* Result, const UWorld* World, FName SubsystemName) = nullptr;
    inline int32_t IOnlineSession_GetSessionSettingsIndex = -1;
    // AFortTeamPrivateInfo::SetHealthAndShield (3.5.1 0xF83CB0, 21.40's name): the player state's CurrentHealth, MaxHealth,
    // CurrentShield and MaxShield into its team's replicated RepData (marking the changed items dirty), which squadmates'
    // squad bars read. The game mode's ReplicateHealthAndShield calls it for every player state each tick; Battle
    // Royale's player bots call it after correcting their values: Finder::ResolveTeamPrivateHealthAndShieldLive.
    inline void (*AFortTeamPrivateInfo_SetHealthAndShield)(AFortTeamPrivateInfo* This, AFortPlayerStateZone* PlayerState) = nullptr;
    // The team private info's add and remove of a player state's RepData entries (8.50 CL-6058028 0x177D900 and
    // 0x177D990; not in the 21.40 exports), which AFortTeamInfo::AddToTeam and RemoveFromTeam call. From 8.30 (CL-5793395
    // to 8.51 CL-6165369 checked) both calls skip a player state with bIsABot ("test byte ptr [rbx+26Eh], 8; jnz"), so a
    // bot has no entry and SetHealthAndShield finds nothing to set: Finder::ResolveTeamPrivatePlayerStateLive, only where
    // that gate is.
    inline void (*AFortTeamPrivateInfo_AddPlayerState)(AFortTeamPrivateInfo* This, AFortPlayerStateZone* PlayerState) = nullptr;
    inline void (*AFortTeamPrivateInfo_RemovePlayerState)(AFortTeamPrivateInfo* This, AFortPlayerStateZone* PlayerState) = nullptr;
    // The game state's step at the end of AFortGameModeAthena::RestartPlayer (7.30 CL-4834550 0xD5A9F0, 7.40 0xD85AF0,
    // 8.00 0xDA5070, 8.20 0xE9E060; no 21.40 name): the controller's pawn's MaxHealth, health, MaxShield and shield as
    // the mutators answer FortGameStateAthena_EvaluateMaxHealth to _EvaluateStartingShield (an island's
    // AFortAthenaMutator_HealthAndShield up to 8.00, its AFortAthenaMutator_TeamSettings in 8.20), set on the pawn when one
    // answers; true then. Finder::ResolveRestartHealthAndShieldLive.
    inline bool (*AFortGameStateAthena_ApplyMutatorHealthAndShield)(AFortGameStateAthena* This, AController* Controller) = nullptr;
    inline int32_t AFortWeapon_ModAmmoIndex = -1;
    inline int32_t AFortWeapon_DestroyedDueToDurabilityIndex = -1;
    // 7.40 (CL-5046157, 0x15A3FF0; no 21.40 name): what takes a weapon whose item has left the inventory out of its pawn's
    // hands. On the server, for a weapon marked bPendingDestroyDueToDurabilityOrStackCount, it queues a Replace of the
    // weapon's item for the owner's client-side quickbars (DelayedQuickBarActions) and destroys the weapon; true when it
    // did. AFortPawn's ability-ended handler (0x13D35F0) calls it: Finder::ResolveWeaponPendingDestroyLive.
    inline bool (*AFortWeapon_HandlePendingDestroy)(AFortWeapon* This) = nullptr;
    inline int32_t IFortInventoryOwnerInterface_GetWorldInventoryIndex = -1;
    inline int32_t IFortInventoryOwnerInterface_RemoveInventoryItemIndex = -1;
    inline int32_t IFortInventoryOwnerInterface_ModDurabilityIndex = -1;
    // 8.50 (CL-6058028): the player controller's (0x16F8F60, 0x16F8ED0) pass the pickup to the game state's
    // OnPickupPreGivenToPlayer / OnPickupPostGivenToPlayer (slots 0x11E, 0x11F), which ask its mutators. Finder::ResolvePickupGivenLive.
    inline int32_t IFortInventoryOwnerInterface_OnPickupPreGivenIndex = -1;
    inline int32_t IFortInventoryOwnerInterface_OnPickupPostGivenIndex = -1;
    // The game state's two slots those call (8.50: 0x11E, 0x11F), found with them.
    inline int32_t AFortGameStateAthena_OnPickupPreGivenToPlayerIndex = -1;
    inline int32_t AFortGameStateAthena_OnPickupPostGivenToPlayerIndex = -1;
    // 8.50: AFortGameModeAthena's (0xFA3A20, slot 0x190): StartingItems, then each mutator's
    // FortGameModeAthena_GetInventoryOverride. Finder::ResolveGetStartingItemsLive.
    inline int32_t AFortGameModeZone_GetStartingItemsIndex = -1;
    // 9.01: Wick's Bounty's FortGameStateAthena_OnClientPawnLoaded (slot 0x116, 0x110ABF0). Finder::ResolveWaxOnClientPawnLoadedLive.
    inline int32_t AFortAthenaMutator_Wax_OnClientPawnLoadedIndex = -1;
    inline int32_t UObject_NeedsLoadForServerIndex = -1;
    inline int32_t UNetDriver_IsLevelInitializedForActorIndex = -1;
    inline int32_t UNetConnection_TickIndex = -1;
    inline int32_t UNetConnection_IsNetReadyIndex = -1;
    inline int32_t UNetConnection_ClientHasInitializedLevelForIndex = -1;
    inline int32_t UNetConnection_FlushDormancyIndex = -1;
    inline int32_t UChannel_CloseIndex = -1;
    inline int32_t UChannel_SendBunchIndex = -1;
    inline int32_t UChannel_StartBecomingDormantIndex = -1;
    inline int32_t UPackageMap_WriteObjectIndex = -1;
    inline int32_t AActor_GetVelocityIndex = -1;
    inline int32_t AActor_GetNetPriorityIndex = -1;
    inline int32_t AActor_GetNetDormancyIndex = -1;
    inline int32_t AActor_IsNetRelevantForIndex = -1;
    inline int32_t AActor_IsRelevancyOwnerForIndex = -1;
    inline int32_t AActor_GetNetOwnerIndex = -1;
    inline int32_t AActor_ForceNetUpdateIndex = -1;
    inline int32_t AController_GetViewTargetIndex = -1;
    inline int32_t APlayerController_SendClientAdjustmentIndex = -1;
    inline int32_t AGameNetworkManager_IsInLowBandwidthModeIndex = -1;
    inline int32_t UEngine_GetMaxTickRateIndex = -1;
    inline int32_t UEngine_InitIndex = -1;
    inline int32_t UConsole_ConsoleCommandIndex = -1;
    inline int32_t UConsole_InputKeyIndex = -1;
    inline int32_t APlayerController_ConsoleKeyIndex = -1;
    inline int32_t APlayerController_SendToConsoleIndex = -1;
    inline int32_t APlayerController_AddCheatsIndex = -1;
    // APlayerController::AddCheats itself (the engine's, from PlayerController's own vtable), for the Super::AddCheats call
    // of AFortPlayerController::AddCheats, which 4.2 compiles out (Finder::ResolvePlayerControllerCheats).
    inline void (*APlayerController_AddCheats)(APlayerController* This, bool bForce) = nullptr;
    inline int32_t UAssetManager_GetPrimaryAssetDataListIndex = -1;
    inline int32_t AFortPlayerController_PushToTalkPressedIndex = -1;
    inline int32_t AFortPlayerController_PushToTalkReleasedIndex = -1;
    inline int32_t APlayerController_ConsoleCommandIndex = -1;
    inline int32_t IConsoleManager_ForEachConsoleObjectThatStartsWithIndex = -1;
    inline int32_t IConsoleManager_FindConsoleVariableIndex = -1;
    inline int32_t UProperty_GetCPPTypeIndex = -1;
    inline int32_t UStruct_InitializeStructIndex = -1;
    inline int32_t UStruct_DestroyStructIndex = -1;
    inline int32_t UMcpProfile_PrepareRequestIndex = -1;
    inline bool UMcpProfile_PrepareRequestTakesObject = false; // 1.8.1/1.8.2: PrepareRequest(IHttpRequest&, uint32)
    inline int32_t FUniqueNetId_IsValidIndex = -1;
    inline int32_t FUniqueNetId_ToStringIndex = -1;
    inline int32_t AGameModeBase_LoginIndex = -1;

    inline ptrdiff_t UEngine_WorldList = -1;
    inline ptrdiff_t UEngine_NetworkFailureEvent = -1;
    inline ptrdiff_t UWorld_NextSwitchCountdown = -1;
    inline ptrdiff_t FWorldContext_ThisCurrentWorld = -1;
    inline ptrdiff_t UGameInstance_WorldContext = -1;
    inline ptrdiff_t FMcpServiceConfig_ServicePermissions = -1;
    inline ptrdiff_t FGameServiceMcp_ServiceConfig = -1;
    inline ptrdiff_t FOnlineSubsystemMcp_McpGameService = -1;
    inline ptrdiff_t UMcpProfileGroup_McpSubsystem = -1;
    // 8.40+: UMcpProfileGroup::Mcp (FMcpProfileOSSAccess*), which holds the subsystem; -1 before.
    inline ptrdiff_t UMcpProfileGroup_Mcp = -1;
    inline ptrdiff_t UMcpProfileGroup_CredentialsAccountId = -1;
    inline ptrdiff_t FProfileHttpRequest_Profile = -1;
    inline ptrdiff_t FProfileHttpRequest_HttpRequest = -1;
    inline ptrdiff_t FProfileHttpRequest_ContextCredentials = -1;
    inline ptrdiff_t FProfileHttpRequest_ResponseType = -1;
    inline ptrdiff_t FWorldContext_RunAsDedicated = -1;
    inline ptrdiff_t FOnlineSubsystemImpl_bForceDedicated = -1;
    inline ptrdiff_t UNetDriver_GuidCache = -1;
    inline ptrdiff_t UNetDriver_bIsPeer = -1;
    inline ptrdiff_t UNetDriver_NetTag = -1;
    inline ptrdiff_t UNetDriver_DebugRelevantActors = -1;
    inline ptrdiff_t UNetDriver_LastPrioritizedActors = -1;
    inline ptrdiff_t UNetDriver_LastRelevantActors = -1;
    inline ptrdiff_t UNetDriver_LastSentActors = -1;
    inline ptrdiff_t UNetDriver_LastNonRelevantActors = -1;
    inline ptrdiff_t UNetDriver_DestroyedStartupOrDormantActors = -1;
    // The map's values are the FActorDestructionInfos themselves up to 3.2, TUniquePtr<FActorDestructionInfo> from 3.3.
    inline bool UNetDriver_DestroyedStartupOrDormantActorsIsUnique = false;
    inline ptrdiff_t UNetDriver_ReplicationFrame = -1;
    inline ptrdiff_t UNetDriver_NetworkObjects = -1;
    inline ptrdiff_t UNetConnection_State = -1;
    inline ptrdiff_t UNetConnection_QueuedBits = -1;
    inline ptrdiff_t UNetConnection_TickCount = -1;
    inline ptrdiff_t UNetConnection_TimeSensitive = -1;
    inline ptrdiff_t UNetConnection_Channels = -1;
    // Channels is UChannel* [MAX_CHANNELS] up to 2.4 and a TArray<UChannel*> (sized by MaxChannelSize) from 2.4.2.
    inline bool UNetConnection_ChannelsIsArray = false;
    inline ptrdiff_t UNetConnection_ActorChannels = -1;
    inline ptrdiff_t UNetConnection_DormantReplicatorMap = -1;
    inline ptrdiff_t UNetConnection_DestroyedStartupOrDormantActors = -1;
    inline ptrdiff_t UNetConnection_ClientWorldPackageName = -1;
    inline ptrdiff_t UNetConnection_ClientVisibleLevelNames = -1;
    inline bool UNetConnection_ClientVisibleLevelNamesIsSet = false;
    inline ptrdiff_t UChannel_Bits = -1;
    inline ptrdiff_t FArchive_ArIsError = -1;
    inline uint32_t FArchive_ArIsErrorMask = 0;
    inline ptrdiff_t FBitWriter_Num = -1;
    inline ptrdiff_t FOutBunch_Size = -1;
    inline ptrdiff_t FOutBunch_bClose = -1;
    inline ptrdiff_t FOutBunch_bDormant = -1;
    inline ptrdiff_t FOutBunch_bReliable = -1;
    // The flags' bits: 1 where they are bool bytes (1.1, 1.2), their bits in the shared flag byte on 1.7.2.
    inline uint32_t FOutBunch_bCloseMask = 0;
    inline uint32_t FOutBunch_bDormantMask = 0;
    inline uint32_t FOutBunch_bReliableMask = 0;
    // 4.22's networking (7.20): EChannelCloseReason CloseReason, after the flag bytes. -1 before it.
    inline ptrdiff_t FOutBunch_CloseReason = -1;
    inline ptrdiff_t UChannel_ChIndex = -1;
    inline ptrdiff_t FFrame_Object = -1;
    inline ptrdiff_t FFastArraySerializer_IDCounter = -1;
    // 5.0: CachedNumItems, then CachedNumItemsToConsiderForWriting (int32 each), for the inlined MarkArrayDirty.
    inline ptrdiff_t FFastArraySerializer_CachedNumItems = -1;
    inline ptrdiff_t AFortPlayerController_InventoryOwnerInterface = -1;
    inline ptrdiff_t UAbilitySystemComponent_GameplayTagAssetInterface = -1;
    inline int32_t IGameplayTagAssetInterface_HasAllMatchingGameplayTagsIndex = -1;
    inline int32_t IGameplayTagAssetInterface_GetOwnedGameplayTagsIndex = -1;
    inline int32_t (*UFortLootLevel_GetItemLevel)(const void* LootLevelData, int32_t WorldLevel) = nullptr;
    inline uintptr_t ABuildingSMActor_PostUpdate = 0;
    inline int32_t ABuildingSMActor_PostUpdateIndex = -1;
    inline bool ABuildingSMActor_bPostUpdateStripped = false;
    inline void (*ABuildingSMActor_SelectMeshSet)(void* This, int32_t LootTier) = nullptr;
    inline int32_t ABuildingSMActor_SetMeshSetIndex = -1;
    inline int32_t ABuildingContainer_IsAlreadySearchedIndex = -1;
    inline int32_t ABuildingContainer_SpawnLootIndex = -1;
    // Whether the game's SpawnLoot call (the container's ServerOnAttemptInteract) passes an EFortPickupSourceTypeFlag
    // after the searcher (7.10 on: "mov r8b, 4", Container), and then an EFortPickupSpawnSource (7.20 on: "xor r9d,
    // r9d", Unset).
    inline bool ABuildingContainer_bSpawnLootTakesSourceTypeFlag = false;
    inline bool ABuildingContainer_bSpawnLootTakesSpawnSource = false;
    // ModifyLootDrops, OnPickupSpawn the slot after it: SpawnLoot's next slot up to 7.20, the one after that from 7.30
    // (AllowsContainerItemCacheLootDrops comes between).
    inline int32_t ABuildingContainer_ModifyLootDropsIndex = -1;
    inline int32_t ABuildingContainer_HideContainerIndex = -1;
    inline int32_t ABuildingContainer_InitializeTierGroupDataIndex = -1;
    inline uintptr_t ABuildingContainer_PostLoad = 0;
    inline int32_t ABuildingContainer_PostLoadIndex = -1;
    inline bool ABuildingContainer_bPostLoadStripped = false;
    inline void (*AFortWorldManager_UpdateActorRecord)(void* This, void* Building, uint8_t State, bool bForce) = nullptr;
    inline int32_t AFortPlayerStateZone_ModifyStatIndex = -1;
    inline int32_t ABuildingSMActor_AttemptSpawnResourcesIndex = -1;
    inline int32_t ABuildingSMActor_DetermineMaxResourcesToSpawnIndex = -1;
    inline int32_t ABuildingSMActor_GetMaximumQuotaLevelBoundIndex = -1;
    inline int32_t ABuildingSMActor_ClearAnimatingMIDsIndex = -1;
    inline uintptr_t ABuildingSMActor_ClearAnimatingMIDs = 0;
    inline void* (*AFortWorldManager_GetTileRecord)(AFortWorldManager* This, void* TileInfo, int32_t TileIndex, void* OutLevelIndices) = nullptr;
    inline ptrdiff_t AFortWorldManager_LootRandomList = -1;
    inline int32_t AFortAIPawn_HandleDeathIndex = -1;
    inline void (*UFortAIEncounterInfo_TryUpgradeSpawnGroup)(void* This, const void* GroupGuid, bool bForEnemySpawner) = nullptr;
    inline int32_t (*UFortAISpawnGroup_GetSpawnGroupCost)(void* SpawnGroup, const void* Upgrade, float DifficultyLevel, int32_t NumPlayers) = nullptr;
    inline void (*FFortAISpawnGroupUpgrade_PickPawnUpgrade)(const void* Upgrade, const void* EnemyTags, int32_t NumPlayers, void* OutPawnUpgrade) = nullptr;
    inline ptrdiff_t FFrame_Code = -1;
    // FFrame::Locals, right after Code (Epic's FFrame: Node, Object, Code, Locals): the frame ProcessEvent copied a native's
    // parameters into.
    inline ptrdiff_t FFrame_Locals = -1;
    inline ptrdiff_t FFrame_PropertyChainForCompiledIn = -1;
    inline ptrdiff_t UChannel_NumOutRec = -1;
    inline ptrdiff_t UActorChannel_RelevantTime = -1;
    inline ptrdiff_t UActorChannel_LastUpdateTime = -1;
    // 3.0: UActorChannel::LastUpdateTime is a TMap<TWeakObjectPtr<AActor>, double> on the connection instead.
    inline ptrdiff_t UNetConnection_LastUpdateTimes = -1;
    inline ptrdiff_t UActorChannel_Bits = -1;
    inline ptrdiff_t FObjectReplicator_OwningChannel = -1;
    inline ptrdiff_t AActor_NetDormancy = -1;
    inline ptrdiff_t AActor_bActorInitialized = -1;
    inline uint32_t AActor_bActorInitializedMask = 0;
    inline ptrdiff_t ULevel_bIsAssociatingLevel = -1;
    inline ptrdiff_t AFortPlayerStateOutpost_bIsOutpostOwner = -1;
    inline uint32_t AFortPlayerStateOutpost_bIsOutpostOwnerMask = 0;
    inline uint32_t ULevel_bIsAssociatingLevelMask = 0;
    inline ptrdiff_t UWorld_TimeSeconds = -1;
    inline ptrdiff_t UConsole_AutoCompleteList = -1;
    inline ptrdiff_t UConsole_bIsRuntimeAutoCompleteUpToDate = -1;
    inline ptrdiff_t UConsole_AutoCompleteTree = -1;
    inline ptrdiff_t UConsole_ConsoleSettings = -1;

    // FTimerManager::ResolveNatives, from UKismetSystemLibrary::K2_SetTimerDelegate: the timer manager pointers
    // UWorld::GetTimerManager reads (the owning game instance's, else the world's own) and FTimerManager::
    // InternalSetTimer(InOutHandle, FTimerUnifiedDelegate&&, InRate, InbLoop, InFirstDelay).
    inline ptrdiff_t UGameInstance_TimerManager = -1;
    inline ptrdiff_t UWorld_TimerManager = -1;
    inline void (*FTimerManager_InternalSetTimer)(void* TimerManager, void* InOutHandle, void* InDelegate, float InRate, bool InbLoop, float InFirstDelay) = nullptr;

    // FTickFunction::ResolveNatives, from FTickTaskManager::Get (the "dumpticks" command its constructor registers):
    // FTickFunction's RegisterTickFunction(ULevel*), UnRegisterTickFunction() and SetTickFunctionEnable(bool), and its
    // bRegistered (a byte from 3.x, Mask 0; a bit of +0xC's bitfield before, 1.8.1's 0x40).
    inline void (*FTickFunction_RegisterTickFunction)(void* This, void* Level) = nullptr;
    inline void (*FTickFunction_UnRegisterTickFunction)(void* This) = nullptr;
    inline void (*FTickFunction_SetTickFunctionEnable)(void* This, bool bInEnabled) = nullptr;
    inline ptrdiff_t FTickFunction_bRegistered = -1;
    inline uint8_t FTickFunction_bRegisteredMask = 0;

    // UAbilitySystemComponent::ResolveNatives: the ability system's unreflected OnActiveGameplayEffectAddedDelegateToSelf
    // (FActiveGameplayEffectsContainer::AddActiveGameplayEffectGrantedTagsAndModifiers broadcasts it last: 1.8.1 +0x2C0,
    // 3.2 +0x2D0, 4.4 +0x220, 5.0 +0x220, 5.21-5.40 +0x210) and FActiveGameplayEffectsContainer::
    // RegisterGameplayAttributeEvent(FGameplayAttribute Attribute), the FindOrAdd on AttributeChangeDelegates that
    // InternalUpdateNumericalAttribute broadcasts (the attribute goes by pointer to a copy the callee destroys).
    inline ptrdiff_t UAbilitySystemComponent_OnActiveGameplayEffectAddedDelegateToSelf = -1;
    inline void* (*FActiveGameplayEffectsContainer_RegisterGameplayAttributeEvent)(void* This, void* Attribute) = nullptr;
    // UWorld::AddOnActorSpawnedHandler: the world's unreflected OnActorSpawned, which UWorld::SpawnActor broadcasts last
    // (1.8.1 +0x390, 3.2, 4.4 and 5.0-5.40 +0x3E0).
    inline ptrdiff_t UWorld_OnActorSpawned = -1;

    // From 5.40 (The Getaway), Finder::ResolveSpecialActorReplicationLive: the game state's special actor list, which
    // every client's map and compass draw (AFortSpecialActorReplicationInfo::AddActorToReplicationList and
    // RemoveActorFromReplicationList(FName UniqueID)), and the constructor and destructor of what Add takes.
    inline int32_t (*AFortSpecialActorReplicationInfo_AddActorToReplicationList)(AFortSpecialActorReplicationInfo* This, const FSpecialActorInitData* InitData) = nullptr;
    inline void (*AFortSpecialActorReplicationInfo_RemoveActorFromReplicationList)(AFortSpecialActorReplicationInfo* This, FName UniqueID) = nullptr;
    inline FSpecialActorInitData* (*FSpecialActorInitData_FSpecialActorInitData)(FSpecialActorInitData* This) = nullptr;
    inline void (*FSpecialActorInitData_Destructor)(FSpecialActorInitData* This) = nullptr;
    // AFortAthenaExitCraftSpawner's unreflected OnExitCraftSpawned, its first member (5.40 and 5.41 +0x328, after AActor).
    inline ptrdiff_t AFortAthenaExitCraftSpawner_OnExitCraftSpawned = -1;
    // Its unreflected DestroyBuildingActorHandle (5.40 +0x370): the handle its BeginPlay sets DestroyBlockingActors'
    // timer with, Finder::DestroyBlockingActorsBinds.
    inline ptrdiff_t AFortAthenaExitCraftSpawner_DestroyBuildingActorHandle = -1;

    // From 6.00 (dynamic foundations, the floating island's), Finder::ResolveBuildingFoundationLive, off
    // ABuildingFoundation::PostUpdate (6.00 0xD11960): SelectAndSetupMyBuildingLevel(RandomList) (0xD142E0, which picks
    // the plot's level and streams it in), and the unreflected bTransformSet and bPostUpdateCalled bits (21.40's names;
    // 6.00 +0xBB1 masks 8 and 4). The bits' byte, as DefineNativeBitfield reads a uint32 from it.
    inline bool (*ABuildingFoundation_SelectAndSetupMyBuildingLevel)(ABuildingFoundation* This, FFortBuildingRandomList* RandomList) = nullptr;
    inline ptrdiff_t ABuildingFoundation_bTransformSet = -1;
    inline uint32_t ABuildingFoundation_bTransformSetMask = 0;
    inline ptrdiff_t ABuildingFoundation_bPostUpdateCalled = -1;
    inline uint32_t ABuildingFoundation_bPostUpdateCalledMask = 0;

    // From 6.20 (the Playground's leaderboards), off UFortPlaysetItemDefinition::SpawnOrGetMinigameScoreRegistry's native
    // (6.21 0xDE8E40): the registry's record of a score by name (6.21 0x1092680, 6.20's in the same place), which seeds the
    // playset's HighScorePresets rows. On the authority it adds the entry (the name, the score, the time, bHasScore),
    // marks it dirty and puts it in the high scores; true when it ranks there.
    inline bool (*AFortMinigameScoreRegistry_RecordScore)(UObject* This, const FString& PlayerName, float Score) = nullptr;

    // From 7.40 (Creative's creature spawner), Finder::ResolveEncounterOverrideCategoryLive: the build's own helper that
    // adds an encounter difficulty option's category option to an FFortEncounterSettings' InjectedOverrideCategories
    // (8.20 0x11CD310, 8.00 0x10B6EA0; not named in 21.40's PDB): the option class's default object makes the category
    // option (an instance of its CategoryClass), which goes on the end of the array. UFortKismetLibrary::
    // ApplyEncounterOptionOverridesFromWaveDataToEncounterSettings calls it for each override its wave data has.
    inline void (*FFortEncounterSettings_InjectOverrideCategory)(UClass* OptionClass, void* EncounterSettings) = nullptr;

    // From 8.40 (Food Fight: Deep Fried), Finder::ResolveFillFloorSetCapHeightLive: ABuildingFillFloor::SetCapHeight
    // (8.40 0xEA91B0), the one callee of the Fill mutator's OnPlaylistLoaded native (0xFE9F00) that starts by writing its
    // float argument to CapHeight ("movss dword ptr [rcx+708h], xmm1").
    inline void (*ABuildingFillFloor_SetCapHeight)(void* This, float NewCapHeight, bool bUpdateMovement) = nullptr;

    // From 3.5 (vending machines), Finder::ResolveItemCollectorSelectionSetupLive, off the collector's BeginPlay (6.21
    // slot 0x5F, 0xD52510): FinishItemSelectionSetup (6.21 0xD5B380), the client half the builds keep: it unbinds the
    // playlist-tables delegate, sets bHasFinishedSelectionSetup and picks the first active input. BeginPlay calls it at
    // once when the playlist's tables are applied (the call Core redirects), else binds it to their delegate.
    inline void (*ABuildingItemCollectorActor_FinishItemSelectionSetup)(UObject* This) = nullptr;

    // From 7.00 (Creative), Finder::ResolveCreativeLive. The island's level record and its cloud save:
    // ULevelSaveRecord::ReadFromBuffer (7.00 0x1089B60, ReadFromDisk's 0x1069320 call with the file's bytes; it reads
    // them through ReadFromArchive 0x10898F0), and UFortLevelSaveComponent's cloud save (0xF5ADB0, the function that logs
    // "Unable to start cloud save. Authority=%s, Busy=%s"): the plot, its owner and bCheckin (false: the sidecar's upload,
    // true: its checkin). USidecarSys's meta setter (0x2DF5C40, called by the save for "serialized-at" and the actor
    // counts) adds the file to the sidecar's FileInfoMap as it sets the key.
    inline bool (*ULevelSaveRecord_ReadFromBuffer)(UObject* This, const TArray<uint8_t>& Buffer) = nullptr;
    inline void (*UFortLevelSaveComponent_CloudSave)(UObject* This, UObject* Plot, const FUniqueNetIdRepl& UserId, bool bCheckin) = nullptr;
    // 7.10 (CL-4667333): the cloud save reads the plot and its owner from the component and takes only bCheckin:
    // bool (UObject* This, bool bCheckin).
    inline bool UFortLevelSaveComponent_CloudSaveTakesCheckinOnly = false;
    // 7.30 (CL-4834550, 0x1096430): bool (UObject* This, ESaveOperation Operation), the plot and its owner the
    // component's (mov r13d, edx). 7.40 (CL-5046157, 0x10BE800) to 9.00 take the record already written out:
    // bool (UObject* This, TArray<uint8>* Data, ESaveOperation Operation) (mov r12d, r8d; 8.20 0x11EEC30, 8.50 0x135B850,
    // 9.00 0x13FD4A0), the buffer from ULevelSaveBaseComponent_SaveLevelRecordToBuffer (8.20 0x133EC80: false without a
    // valid record), as the periodic save (8.20 0x11EDB60) and EndPlay (0x11D4F40) call them.
    namespace ESaveOperation
    {
        inline constexpr uint32_t Save = 0;
        inline constexpr uint32_t SaveAndCheckIn = 1;
        // EndPlay's for a volume in a game (ReadOnly): the file checked in unsaved ("Unlocking level record").
        inline constexpr uint32_t CheckIn = 2;
    }
    inline bool UFortLevelSaveComponent_CloudSaveTakesOperation = false;
    inline bool UFortLevelSaveComponent_CloudSaveTakesData = false;
    inline bool (*ULevelSaveBaseComponent_SaveLevelRecordToBuffer)(UObject* This, TArray<uint8_t>& Data) = nullptr;
    inline void (*USidecarSys_SetMeta)(UObject* This, const FString& AppName, const FString& FilePath, const FString& Key, const FString& Value) = nullptr;
    // From 7.00: AFortPlayerControllerAthena::ServerLoadPlotForPortal_Implementation (0xC65E80, slot 0x3E6), the game's
    // own load of a plot into a rift: the call Core redirects to AFortAthenaCreativePortal::LoadPlotForUser is in it.
    // And the plot item's id getter it uses (UMcpItem's virtual, slot 0x83 on 7.00), which names the island's file.
    inline void (*AFortPlayerControllerAthena_ServerLoadPlotForPortal)(UObject* This, UObject* Portal, const FString& PlotItemId) = nullptr;
    inline int32_t UMcpItem_GetItemIdIndex = -1;
    // From 7.00 (Creative), AFortPlayerControllerAthena::HookCreativeVTables. The player's island link
    // (CreativePlotLinkedVolume) setter, the function that binds the controller's OnMinigameVolumeStateChanged to the
    // island (7.00 0xC67A90, 7.10 0xC866B0, 7.20 0xD63370): every build keeps only its EndPlay call with null, the
    // server's calls with an island are stripped. And the rift refresh it ends with for the old island's rifts (7.00
    // 0xBCBBB0, 7.10 0xBE3800: VolumeDisplayName, bPortalOpen and CurrentPopulation, the controllers linked to the rift's
    // island); its name isn't in the 21.40 exports.
    inline void (*AFortPlayerControllerAthena_SetCreativePlotLinkedVolume)(UObject* This, UObject* Volume) = nullptr;
    inline void (*AFortAthenaCreativePortal_RefreshPortal)(UObject* This) = nullptr;
    // From 7.00 (Creative), AFortPlayerControllerAthena::HookCreativeVTables. UMcpProfile::GetStat (0x9530D0: the profile
    // state's stat by name, a TSharedPtr<FJsonValue> returned through OutValue), the call the creative profile's plot
    // permissions getter (0x1132C70) makes with "creative_plot_permissions"; and the offset of UMcpProfile's
    // ItemsUpdatedDelegate (+0xF8), the delegate UMcpProfile::HandleProfileUpdate (0x955BE0) broadcasts last.
    inline void* (*UMcpProfile_GetStat)(const UMcpProfile* This, void* OutValue, const FString& StatName) = nullptr;
    inline ptrdiff_t UMcpProfile_ItemsUpdatedDelegate = -1;
    // 8.20 (CL-5625478) keeps ItemsUpdatedDelegate in an unreflected object the profile points to: HandleProfileUpdate
    // (0xBBFEC0) passes [profile+0xE8] to 0xBB3030, which broadcasts +0x108 of it last (add rcx, 108h; jmp 0x3135A0).
    // UMcpProfile_ItemsUpdatedDelegate is then the offset inside that object. -1 where the delegate is the profile's own.
    inline ptrdiff_t UMcpProfile_ItemsUpdatedDelegateHolder = -1;
    // From 7.00 (Creative), AFortPlayerControllerAthena::HookCreativeVTables: what an island's load runs on and with, the
    // way the game's own loads and resets do (UFortLevelSaveComponent::BeginRestrictedPlotLoad 0xF363B0,
    // AFortVolumeManager::ResetVolumeAndLevelRecord 0xDA0E70). AFortVolume::GetTaskQueue (0xD8F7A0; 7.10 0xDB2400), the
    // reset's first call; UAsyncTaskQueue::Add (0xBA90E0; 7.10 0xBBED60: the owner's weak pointer and the step's
    // delegate, both by reference), the call the reset makes once per step, the one of them that makes an object;
    // UAsyncTaskQueue's start (0xBADE10; 7.10 0xBC3E90: cmp qword ptr [rcx+CurrentTask], 0; je ExecuteNextTask), the
    // queue's ExecuteNextTask being the function that binds "&UAsyncTaskQueue::OnAsyncTaskFinished" (0xBADFD0).
    inline UObject* (*AFortVolume_GetTaskQueue)(UObject* This) = nullptr;
    inline void (*UAsyncTaskQueue_Add)(UObject* This, const FWeakObjectPtr& Owner, const void* Task) = nullptr;
    inline void (*UAsyncTaskQueue_Start)(UObject* This) = nullptr;
    // The game's own queue steps (each returns the dynamic delegate its end broadcasts, or null when it is done at once):
    // the volume's clear (0xD83860; 7.10 0xDA5ED0), the reset's step that returns OnFinishVolumeClear;
    // UFortLevelSaveComponent::LoadPlaysetAsync (0xF54660; 7.10 0xF7EA90, "Start playset async load for user %s,
    // cloudsave=%s."): the plot's playset loaded, then OnAsyncLoadingOfPlaysetComplete; UPlaysetLevelStreamComponent's
    // LoadPlaysetAsyncNoCallback (0x1325850; 7.10 0x1364750) and UnloadPlaysetAsyncNoCallback (0x1338210; 7.10
    // 0x13775D0), the callers of LoadPlayset ("UPlaysetLevelStreamComponent::LoadPlayset Error: no owner for %s") and
    // UnloadPlayset ("UPlaysetLevelStreamComponent::UnloadPlayset %s") that return lea rax, [rbx+disp32] after the call;
    // on 7.20 both are tail calls (0x144B740: jmp LoadPlayset; 0x145B140: mov dl, 1; jmp UnloadPlayset).
    inline void* (*AFortVolume_ClearVolumeOnServer)(UObject* This) = nullptr;
    // 7.10+ (0xF7EA90; 7.20 0x10646C0) take the playset to load, a TSoftObjectPtr<UFortPlaysetItemDefinition> by value
    // (the plot's, the payload BeginRestrictedPlotLoad binds the step with; 7.20 0x1068AF0), where 7.00 reads the plot's
    // itself: LoadPlaysetAsyncTakesPlayset, from the function keeping rdx before its first call (mov rdi, rdx).
    inline void* (*UFortLevelSaveComponent_LoadPlaysetAsync)(UObject* This, void* PlaysetToLoad) = nullptr;
    inline bool UFortLevelSaveComponent_LoadPlaysetAsyncTakesPlayset = false;
    inline void* (*UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback)(UObject* This) = nullptr;
    inline void* (*UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback)(UObject* This) = nullptr;
    // 7.30 (CL-4834550) has no NoCallback steps: the island's own steps bind UPlaysetLevelStreamComponent::LoadPlayset
    // (0x148A5A0) as the load step, which LoadPlaysetAsyncNoCallback then holds, and UnloadPlayset (0x149A330) with a
    // true payload as the unload step (BeginRestrictedPlotLoad 0x106A880: mov byte ptr [rbx+20h], 1 at 0x106ACEC). The
    // bool's name isn't in 21.40.
    inline void* (*UPlaysetLevelStreamComponent_UnloadPlayset)(UObject* This, bool bForce) = nullptr;
    // UFortLevelSaveComponent::ResetLevelRecord (0xF5AAC0; 7.10 0xF88310, the function that sets the island file's
    // "last-reset" meta): the base's reset (0x108BBA0: the volume's playset's own record, else a new one; bValidRecord)
    // and the file's reset time. The reset's step of the same name calls it.
    inline void (*UFortLevelSaveComponent_ResetLevelRecord)(UObject* This) = nullptr;
    // ULevelSaveRecord's end of a read (0x1073950; 7.10 0x10A37A0; Core's name, after the reflected NotifyFinishedSpawning):
    // the records tidied after the read, the read flag cleared and OnFinishedRead broadcast (bSucceeded, else
    // "read_failed"). The other function that logs "read_failed" is ReadFromDisk.
    inline void (*ULevelSaveRecord_NotifyFinishedRead)(UObject* This, bool bSucceeded) = nullptr;
    // From 7.10 (CL-4667333; PongooDev/CoreV2#488), AFortPlayerControllerAthena::HookCreativeIslandTools: the component's
    // load of a published island by its code (0xF7EF90, the function that logs "Attempting to load plot from mnemonic
    // %s"; its BeginPlay, 0xF5E610, calls it for the featured rifts' CuratedLinkCodes): (this, the user's id, the code).
    inline void (*UFortLevelSaveComponent_LoadPlotFromMnemonic)(UObject* This, const FUniqueNetIdRepl& UserId, const FString& Mnemonic) = nullptr;
    // From 7.30 (CL-4834550), UFortLevelSaveComponent::HookPermissions: the end of an island's permissions change, the
    // game's own (21.40's UPlayspaceComponent_CreativeToolsPermission::OnModifyCreativePlotPermissionsComplete; 7.30
    // 0x108EE70, 7.40 0x10B6800, the function that logs "Error changing perms %s"): the MCP command's result and the new
    // permissions, which the completion delegate carries as its payload by value (the function destroys them).
    inline void (*UFortLevelSaveComponent_OnModifyCreativePlotPermissionsComplete)(UObject* This, const void* Result, void* NewPermissions) = nullptr;
}

namespace Sites
{
    struct FInlinedNetMode
    {
        uint8_t* Branch = nullptr;
        uint8_t* DemoNetDriverDisplacement = nullptr;
        uint8_t NetDriverDisplacement = 0;
    };

    inline uint8_t* PreInitRegularClient = nullptr;
    inline std::vector<uint8_t*> PreInitDedicatedServerChecks;
    inline std::vector<uint8_t*> EngineInitDedicatedServerChecks;
    inline std::vector<uint8_t*> GameEngineTickDedicatedServerChecks;
    inline std::vector<void**> ConsoleVTables;
    inline std::vector<void**> NetDriverVTables;
    inline uint8_t* ActorTickReceiveTick = nullptr;
    inline uint8_t* ShouldUseThreadingForPerformanceDedicatedCheck = nullptr;
    inline uint8_t* LinkerFilterClientButNotServer = nullptr;
    // 5.0 (UE 4.21): the store is mov [base+disp32], r8b-r15b (REX 88) rather than mov byte ptr [...], 1.
    inline bool LinkerFilterRegisterStore = false;
    inline std::vector<uint8_t*> ChoosePlayerStartGoalManagerCalls;
    inline std::vector<uint8_t*> AIDirectorGetCurrentCalls;
    inline uint8_t* ShouldUseIdleModeCheck = nullptr;
    inline uint8_t* ShouldUseIdleModeFalse = nullptr;
    // 5.0 (UE 4.21): ShouldUseIdleMode is inlined into FEngineLoop::Tick; the jz past bIdleMode = true (not patched:
    // its presence makes Core set t.IdleWhenNotForeground to 0).
    inline uint8_t* ShouldUseIdleModeInlinedBranch = nullptr;
    inline uint8_t* TeamMemberInfoAthenaMenuHeroRand = nullptr;

    struct FMaxTickRateWait
    {
        uint8_t* SleepThreshold = nullptr;
        uint8_t* SleepSlack = nullptr;
        uint8_t* SpinLoop = nullptr;
    };
    inline FMaxTickRateWait MaxTickRateWait;
    inline std::vector<uint8_t*> FlushActorDormancyCalls;
    inline uint8_t* FortGameInstanceStartDedicatedCheck = nullptr;
    inline uint8_t* FortGameInstanceStartDedicatedTarget = nullptr;
    // UFortHotfixManager's after-hotfix handler (8.00 0x12BB020): its call that hashes the keychain it built from
    // [HotfixedEncryptionKeys], right before it gets the encryption manager and calls MergeInKeyChain.
    inline uint8_t* FortHotfixManagerKeyChainHashCall = nullptr;

    struct FShowFlag
    {
        uint32_t Index = 0;
        const wchar_t* Name = nullptr;
    };

    inline std::vector<FShowFlag> ShowFlags;
    inline uint8_t* LoadMapListenCall = nullptr;
    inline FInlinedNetMode InlinedNetMode[4] = {};
}
