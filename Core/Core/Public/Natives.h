#pragma once

#include <cstddef>
#include <cstdint>

class FString;
class FUObjectArray;
class UClass;
class UEngine;
class UFunction;
class UGameEngine;
class UGameInstance;
class UNetDriver;
class UObject;
class UWorld;
struct FLevelCollection;
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
    inline bool* GIsRunning = nullptr;
    inline uint32_t* GGameThreadId = nullptr;
    inline bool* GIsGameThreadIdInitialized = nullptr;

    inline FName* (*FName_Construct)(FName* This, const wchar_t* Name, int32_t FindType) = nullptr;
    inline void (*FName_AppendString)(const FName* This, FString& Out) = nullptr;
    inline FString* (*FName_ToString)(const FName* This, FString* Out) = nullptr;

    inline void (*FMsg_Logf_Internal)(const char* File, int32_t Line, const FName& Category, uint8_t Verbosity, const wchar_t* Format, ...) = nullptr;

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
    inline void (*UGameEngine_Tick)(UGameEngine* This, float DeltaSeconds, bool bIdleMode) = nullptr;
    inline bool (*UEngine_LoadMap)(UEngine* This, FWorldContext& WorldContext, FURL* URL, void* Pending, FString& Error) = nullptr;
    inline bool (*UEngine_CreateNamedNetDriver)(UEngine* This, UWorld* InWorld, FName NetDriverName, FName NetDriverDefinition) = nullptr;
    inline UNetDriver* (*UEngine_FindNamedNetDriver)(UEngine* This, const UWorld* InWorld, FName NetDriverName) = nullptr;
    inline void (*UEngine_DestroyNamedNetDriver)(UEngine* This, UWorld* InWorld, FName NetDriverName) = nullptr;
    inline FWorldContext* (*UEngine_GetWorldContextFromWorldChecked)(UEngine* This, const UWorld* InWorld) = nullptr;
    inline void (*FOnNetworkFailure_Broadcast)(void* Delegate, UWorld* World, UNetDriver* NetDriver, int32_t FailureType, const FString& ErrorString) = nullptr;

    inline int32_t (*UWorld_InternalGetNetMode)(const UWorld* This) = nullptr;
    inline int32_t (*UWorld_AttemptDeriveFromURL)(const UWorld* This) = nullptr;
    inline FLevelCollection* (*UWorld_FindCollectionByType)(UWorld* This, int32_t InType) = nullptr;
    inline int32_t (*UNetDriver_GetNetMode)(const UNetDriver* This) = nullptr;
    inline void (*UNetDriver_SetWorld)(UNetDriver* This, UWorld* InWorld) = nullptr;

    inline bool (*FURL_HasOption)(const FURL* This, const wchar_t* Test) = nullptr;
    inline FString* (*FURL_ToString)(const FURL* This, FString* Result, bool FullyQualified) = nullptr;

    inline int32_t ProcessEventIndex = -1;
    inline int32_t UNetDriver_InitListenIndex = -1;
    inline int32_t UNetDriver_TickFlushIndex = -1;
    inline int32_t UNetDriver_IsServerIndex = -1;
    inline int32_t UNetDriver_ServerReplicateActorsIndex = -1;
    inline int32_t UEngine_LoadMapIndex = -1;

    inline ptrdiff_t UEngine_WorldList = -1;
    inline ptrdiff_t UEngine_NetworkFailureEvent = -1;
    inline ptrdiff_t UWorld_NextSwitchCountdown = -1;
    inline ptrdiff_t FWorldContext_ThisCurrentWorld = -1;
}

namespace Sites
{
    struct FInlinedNetMode
    {
        uint8_t* Branch = nullptr;
        uint8_t* DemoNetDriverDisplacement = nullptr;
    };

    inline uint8_t* PreInitRegularClient = nullptr;
    inline uint8_t* LoadMapListenCall = nullptr;
    inline FInlinedNetMode InlinedNetMode[4] = {};
}
