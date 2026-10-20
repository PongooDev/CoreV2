#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "Templates/SharedPointerInternals.h"

class FOutputDevice;
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
class AFortGameModeZone;
class AFortLobbyBeaconHost;
class AFortPlayerController;
class AFortPlayerState;
class AFortWorldManager;
class APawn;
class FGameServiceMcp;
class FOnlineSubsystemImpl;
class FOnlineSubsystemMcp;
class FUniqueNetId;
class IHttpRequest;
class UMcpProfileGroup;
struct FMcpServiceConfig;
struct FProfileHttpRequest;
class FString;
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

    inline int (*CRT_rand)() = nullptr;
    inline float (*FMath_SRand)() = nullptr;
    inline int32_t (*FUObjectArray_AllocateSerialNumber)(FUObjectArray* This, int32_t Index) = nullptr;
    inline FOutputDeviceRedirector* (*GetGlobalLogSingleton)() = nullptr;
    inline void (*FOutputDeviceRedirector_AddOutputDevice)(FOutputDeviceRedirector* This, FOutputDevice* OutputDevice) = nullptr;

    inline const wchar_t* (*FPlatformMisc_GameDir)() = nullptr;
    inline const wchar_t* (*FPlatformProcess_UserName)(bool bOnlyAlphaNumeric) = nullptr;
    inline bool (*FPackageName_FindPackagesInDirectory)(TArray<FString>& OutPackages, const FString& RootDir) = nullptr;
    inline FString* FPackageName_MapPackageExtension = nullptr;
    inline IConsoleManager** IConsoleManager_Singleton = nullptr;
    inline void (*IConsoleManager_SetupSingleton)() = nullptr;
    inline FTextLocalizationManager** GTextLocalizationManager = nullptr;
    inline void* (*FTextLocalizationManager_FindDisplayString)(const FTextLocalizationManager* This, void* Result, const FString* Namespace, const FString* Key, const FString* SourceString) = nullptr;
    inline void* UConsole_RegisterConsoleAutoCompleteEntries = nullptr;

    inline void (*FMsg_Logf_Internal)(const char* File, int32_t Line, const FName& Category, int32_t Verbosity, const wchar_t* Format, ...) = nullptr;
    inline void (*FLogCategoryBase_FLogCategoryBase)(FLogCategoryBase* This, const wchar_t* CategoryName, uint8_t InDefaultVerbosity, uint8_t InCompileTimeVerbosity) = nullptr;
    inline void (*FDebug_AssertFailed)(const char* Expr, const char* File, int32_t Line, const wchar_t* Format, ...) = nullptr;
    inline void (*LowLevelFatalErrorHandler)(const char* File, int32_t Line, const wchar_t* Format, ...) = nullptr;

    inline UObject* (*StaticFindObject)(UClass* Class, UObject* InOuter, const wchar_t* Name, bool ExactClass) = nullptr;
    inline UObject* (*StaticLoadObject)(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename, uint32_t LoadFlags, void* Sandbox, bool bAllowObjectReconciliation) = nullptr;
    inline UObject* (*StaticConstructObject_Internal)(UClass* Class, UObject* InOuter, FName Name, int32_t SetFlags, int32_t InternalSetFlags, UObject* Template, bool bCopyTransientsFromClassDefaults, void* InstanceGraph, bool bAssumeTemplateIsArchetype) = nullptr;
    inline void (*CollectGarbage)(int32_t KeepFlags, bool bPerformFullPurge) = nullptr;

    inline const wchar_t* (*FCommandLine_Get)() = nullptr;
    inline bool (*FParse_Param)(const wchar_t* Stream, const wchar_t* Param) = nullptr;

    inline void (*GetSourceIniHierarchyFilenames)(const wchar_t* InBaseIniName, const wchar_t* InPlatformName, const wchar_t* EngineConfigDir, const wchar_t* SourceConfigDir, void* OutHierarchy, bool bRequireDefaultIni) = nullptr;
    inline FString* (*UGameMapsSettings_GetGameDefaultMap)(FString* Result) = nullptr;
    inline const FString* (*UGameMapsSettings_GetGlobalDefaultGameMode)() = nullptr;

    inline void (*UGameInstance_StartGameInstance)(UGameInstance* This) = nullptr;
    inline bool (*UGameInstance_IsDedicatedServerInstance)(const UGameInstance* This) = nullptr;
    inline bool (*UObject_CanCreateInCurrentContext)(UObject* Template) = nullptr;
    inline UClass** (*AFortGameMode_GetGameSessionClass)(const AFortGameMode* This, UClass** Result) = nullptr;
    inline APawn* (*AFortGameMode_SpawnDefaultPawnFor)(AFortGameMode* This, AController* NewPlayer, AActor* StartSpot) = nullptr;
    inline APawn* (*AFortGameModeZone_SpawnDefaultPawnFor)(AFortGameModeZone* This, AController* NewPlayer, AActor* StartSpot) = nullptr;
    inline void (*AFortGameMode_FinishWorldInitialization)(AFortGameMode* This, AFortWorldManager* WorldManager) = nullptr;
    inline void (*AFortWorldManager_OnWorldRecordQueryComplete)(AFortWorldManager* This) = nullptr;
    inline void (*AFortLobbyBeaconHost_SetWorldRecordLoaded)(AFortLobbyBeaconHost* This, bool bInWorldRecordLoaded) = nullptr;
    inline bool (*AFortPlayerController_InitializeMcpProfile)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    inline void (*AFortPlayerState_SetHeroType)(AFortPlayerState* This, UFortHero* Hero) = nullptr;
    inline void (*UFortGlobals_ApplyHeroEffectsAndAbilities)(UFortHero* Hero, IAbilitySystemInterface* AbilitySystem, TArray<FActiveGameplayEffectHandle>& OutEffectHandles, FScriptArray& OutAbilitySetHandles) = nullptr;
    inline bool (*UAbilitySystemComponent_InternalTryActivateAbility)(UAbilitySystemComponent* This, FGameplayAbilitySpecHandle AbilityToActivate, const void* InPredictionKey, UGameplayAbility** OutInstancedAbility, void* OnGameplayAbilityEndedDelegate, const FGameplayEventData* TriggerEventData) = nullptr;
    inline void (*UAbilitySystemComponent_MarkAbilitySpecDirty)(UAbilitySystemComponent* This, FGameplayAbilitySpec& Spec) = nullptr;
    inline void (*UAbilitySystemComponent_IncrementAbilityListLock)(UAbilitySystemComponent* This) = nullptr;
    inline void (*UAbilitySystemComponent_DecrementAbilityListLock)(UAbilitySystemComponent* This) = nullptr;
    inline void* SharedEmptyFunction = nullptr;
    inline void (*FortMcpConfig_UpdateServicePermissions)(void* Environment, void* Context, FMcpServiceConfig* ServiceConfig, void* Extra) = nullptr;
    inline void (*UMcpProfileGroup_SendRequestNow)(UMcpProfileGroup* This, FProfileHttpRequest& Request) = nullptr;
    inline bool (*FOnlineSubsystemMcp_QueueRequestForUser)(FOnlineSubsystemMcp* This, FGameServiceMcp& Service, const FUniqueNetId& UserId, const TSharedRef<IHttpRequest>& HttpRequest, FString& OutError) = nullptr;
    inline bool (*FOnlineSubsystemMcp_QueueRequestForClient)(FOnlineSubsystemMcp* This, FGameServiceMcp& Service, const FString& ClientId, const TSharedRef<IHttpRequest>& HttpRequest, FString& OutError) = nullptr;
    inline bool (*FOnlineSubsystemImpl_IsDedicated)(const FOnlineSubsystemImpl* This) = nullptr;
    inline void (*UGameEngine_Tick)(UGameEngine* This, float DeltaSeconds, bool bIdleMode) = nullptr;
    inline bool (*UEngine_LoadMap)(UEngine* This, FWorldContext& WorldContext, FURL* URL, void* Pending, FString& Error) = nullptr;
    inline bool (*UEngine_CreateNamedNetDriver)(UEngine* This, UWorld* InWorld, FName NetDriverName, FName NetDriverDefinition) = nullptr;
    inline UNetDriver* (*UEngine_FindNamedNetDriver)(UEngine* This, const UWorld* InWorld, FName NetDriverName) = nullptr;
    inline void (*UEngine_DestroyNamedNetDriver)(UEngine* This, UWorld* InWorld, FName NetDriverName) = nullptr;
    inline FWorldContext* (*UEngine_GetWorldContextFromWorldChecked)(UEngine* This, const UWorld* InWorld) = nullptr;
    inline void (*FOnNetworkFailure_Broadcast)(void* Delegate, UWorld* World, UNetDriver* NetDriver, int32_t FailureType, const FString& ErrorString) = nullptr;

    inline ULocalPlayer* (*UGameViewportClient_SetupInitialLocalPlayer)(UGameViewportClient* This, FString& OutError) = nullptr;

    inline bool (*UActorChannel_ReplicateActor)(UActorChannel* This) = nullptr;
    inline void (*UActorChannel_SetChannelActor)(UActorChannel* This, AActor* InActor) = nullptr;
    inline UChannel* (*UNetConnection_CreateChannel)(UNetConnection* This, int32_t Type, bool bOpenedLocally, int32_t ChannelIndex) = nullptr;
    inline FNetViewer* (*FNetViewer_FNetViewer)(FNetViewer* This, UNetConnection* InConnection, float DeltaSeconds) = nullptr;
    inline void (*AActor_CallPreReplication)(AActor* This, UNetDriver* NetDriver) = nullptr;
    inline void (*AActor_ReceiveTick)(AActor* This, float DeltaSeconds) = nullptr;
    inline UNetDriver* (*AActor_GetNetDriver)(const AActor* This) = nullptr;
    inline void (*AActor_FlushNetDormancy)(AActor* This) = nullptr;
    inline void (*AActor_SetNetDormancy)(AActor* This, int32_t NewDormancy) = nullptr;
    inline void (*FNetworkObjectList_Remove)(FNetworkObjectList* This, AActor* Actor) = nullptr;
    inline bool (*FNetGUIDCache_SupportsObject)(const FNetGUIDCache* This, const UObject* Object) = nullptr;
    inline UObject* (*UObject_GetArchetypeFromRequiredInfo)(UClass* Class, UObject* Outer, FName Name, int32_t ObjectFlags) = nullptr;
    inline AWorldSettings* (*UWorld_GetWorldSettings)(const UWorld* This, bool bCheckStreamingPersistent, bool bChecked) = nullptr;
    inline void (*UWorld_AddNetworkActor)(UWorld* This, AActor* Actor) = nullptr;
    inline AActor* (*UWorld_SpawnActor)(UWorld* This, UClass* Class, const FTransform* UserTransformPtr, const FActorSpawnParameters& SpawnParameters) = nullptr;
    inline void (*UScriptStruct_CopyScriptStruct)(const UScriptStruct* This, void* Dest, const void* Src, int32_t ArrayDim) = nullptr;
    inline void (*FFrame_Step)(FFrame* This, UObject* Context, void* const Result) = nullptr;
    inline void (*FFastArraySerializer_MarkArrayDirty)(FFastArraySerializer* This) = nullptr;
    inline void (*FFrame_StepExplicitProperty)(FFrame* This, void* const Result, UProperty* Property) = nullptr;
    inline bool (*UNetDriver_IsAdaptiveNetUpdateFrequencyEnabled)() = nullptr;
    inline void (*UNetDriver_TickFlush)(UNetDriver* This, float DeltaSeconds) = nullptr;
    inline FOutBunch* (*FOutBunch_FOutBunch)(FOutBunch* This, UChannel* InChannel, bool bClose) = nullptr;
    inline bool (*FObjectReplicator_ValidateAgainstState)(FObjectReplicator* This, const UObject* ObjectState) = nullptr;

    inline int32_t (*UWorld_InternalGetNetMode)(const UWorld* This) = nullptr;
    inline int32_t (*UWorld_AttemptDeriveFromURL)(const UWorld* This) = nullptr;
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
    inline int32_t UEngine_LoadMapIndex = -1;
    inline int32_t UConsole_BuildRuntimeAutoCompleteListIndex = -1;
    inline int32_t UObject_NeedsLoadForClientIndex = -1;
    inline int32_t UObject_GetWorldIndex = -1;
    inline int32_t AFortGameMode_FinishWorldInitializationIndex = -1;
    inline int32_t UAbilitySystemComponent_InternalServerTryActiveAbilityIndex = -1;
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
    inline int32_t UConsole_ConsoleCommandIndex = -1;
    inline int32_t UConsole_InputKeyIndex = -1;
    inline int32_t APlayerController_ConsoleKeyIndex = -1;
    inline int32_t APlayerController_SendToConsoleIndex = -1;
    inline int32_t IConsoleManager_ForEachConsoleObjectThatStartsWithIndex = -1;
    inline int32_t IConsoleManager_FindConsoleVariableIndex = -1;
    inline int32_t UProperty_GetCPPTypeIndex = -1;
    inline int32_t UStruct_InitializeStructIndex = -1;
    inline int32_t UStruct_DestroyStructIndex = -1;
    inline int32_t UMcpProfile_PrepareRequestIndex = -1;
    inline int32_t FUniqueNetId_IsValidIndex = -1;

    inline ptrdiff_t UEngine_WorldList = -1;
    inline ptrdiff_t UEngine_NetworkFailureEvent = -1;
    inline ptrdiff_t UWorld_NextSwitchCountdown = -1;
    inline ptrdiff_t FWorldContext_ThisCurrentWorld = -1;
    inline ptrdiff_t UGameInstance_WorldContext = -1;
    inline ptrdiff_t FMcpServiceConfig_ServicePermissions = -1;
    inline ptrdiff_t FGameServiceMcp_ServiceConfig = -1;
    inline ptrdiff_t FOnlineSubsystemMcp_McpGameService = -1;
    inline ptrdiff_t UMcpProfileGroup_McpSubsystem = -1;
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
    inline ptrdiff_t UNetDriver_ReplicationFrame = -1;
    inline ptrdiff_t UNetDriver_NetworkObjects = -1;
    inline ptrdiff_t UNetConnection_State = -1;
    inline ptrdiff_t UNetConnection_QueuedBits = -1;
    inline ptrdiff_t UNetConnection_TickCount = -1;
    inline ptrdiff_t UNetConnection_TimeSensitive = -1;
    inline ptrdiff_t UNetConnection_Channels = -1;
    inline ptrdiff_t UNetConnection_ActorChannels = -1;
    inline ptrdiff_t UNetConnection_DormantReplicatorMap = -1;
    inline ptrdiff_t UNetConnection_DestroyedStartupOrDormantActors = -1;
    inline ptrdiff_t UNetConnection_ClientWorldPackageName = -1;
    inline ptrdiff_t UNetConnection_ClientVisibleLevelNames = -1;
    inline ptrdiff_t UChannel_Bits = -1;
    inline ptrdiff_t FArchive_ArIsError = -1;
    inline uint32_t FArchive_ArIsErrorMask = 0;
    inline ptrdiff_t FBitWriter_Num = -1;
    inline ptrdiff_t FOutBunch_Size = -1;
    inline ptrdiff_t FOutBunch_bClose = -1;
    inline ptrdiff_t FOutBunch_bDormant = -1;
    inline ptrdiff_t FOutBunch_bReliable = -1;
    inline ptrdiff_t UChannel_ChIndex = -1;
    inline ptrdiff_t FFrame_Object = -1;
    inline ptrdiff_t FFastArraySerializer_IDCounter = -1;
    inline ptrdiff_t FFrame_Code = -1;
    inline ptrdiff_t FFrame_PropertyChainForCompiledIn = -1;
    inline ptrdiff_t UChannel_NumOutRec = -1;
    inline ptrdiff_t UActorChannel_RelevantTime = -1;
    inline ptrdiff_t UActorChannel_LastUpdateTime = -1;
    inline ptrdiff_t UActorChannel_Bits = -1;
    inline ptrdiff_t FObjectReplicator_OwningChannel = -1;
    inline ptrdiff_t AActor_NetDormancy = -1;
    inline ptrdiff_t AActor_bActorInitialized = -1;
    inline uint32_t AActor_bActorInitializedMask = 0;
    inline ptrdiff_t ULevel_bIsAssociatingLevel = -1;
    inline uint32_t ULevel_bIsAssociatingLevelMask = 0;
    inline ptrdiff_t UWorld_TimeSeconds = -1;
    inline ptrdiff_t UConsole_AutoCompleteList = -1;
    inline ptrdiff_t UConsole_bIsRuntimeAutoCompleteUpToDate = -1;
    inline ptrdiff_t UConsole_AutoCompleteTree = -1;
    inline ptrdiff_t UConsole_ConsoleSettings = -1;
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
    inline std::vector<uint8_t*> ChoosePlayerStartGoalManagerCalls;
    inline uint8_t* ShouldUseIdleModeCheck = nullptr;
    inline uint8_t* ShouldUseIdleModeFalse = nullptr;

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

    struct FShowFlag
    {
        uint32_t Index = 0;
        const wchar_t* Name = nullptr;
    };

    inline std::vector<FShowFlag> ShowFlags;
    inline uint8_t* LoadMapListenCall = nullptr;
    inline FInlinedNetMode InlinedNetMode[4] = {};
}
