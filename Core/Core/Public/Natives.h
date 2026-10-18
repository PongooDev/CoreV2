#pragma once

#include <cstdint>

class FString;
class UClass;
class UEngine;
class UFunction;
class UNetDriver;
class UObject;
class UWorld;
struct FLogCategoryBase;
struct FName;
struct FUObjectArray;
struct FWorldContext;

namespace Natives
{
    inline void* (*FMemory_Malloc)(size_t Count, uint32_t Alignment) = nullptr;
    inline void* (*FMemory_Realloc)(void* Original, size_t Count, uint32_t Alignment) = nullptr;
    inline void (*FMemory_Free)(void* Original) = nullptr;

    inline FName* (*FName_Construct)(FName* This, const wchar_t* Name, int32_t FindType) = nullptr;
    inline void (*FName_AppendString)(const FName* This, FString& Out) = nullptr;

    inline void (*FMsg_Logf_Internal)(const char* File, int32_t Line, const FLogCategoryBase& Category, uint8_t Verbosity, const wchar_t* Format, ...) = nullptr;

    inline UObject* (*StaticFindObject)(UClass* Class, UObject* InOuter, const wchar_t* Name, bool bExactClass) = nullptr;
    inline UObject* (*StaticLoadObject)(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename, uint32_t LoadFlags, void* Sandbox, bool bAllowObjectReconciliation) = nullptr;

    inline FUObjectArray* GUObjectArray = nullptr;
    inline void** GNames = nullptr;
    inline UWorld** GWorld = nullptr;
    inline UEngine** GEngine = nullptr;
    inline bool* GIsClient = nullptr;
    inline bool* GIsServer = nullptr;

    inline int32_t ProcessEventIndex = -1;
}
