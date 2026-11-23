#pragma once

#include "UObject/Class.h"

#define ANY_PACKAGE (reinterpret_cast<UObject*>(-1))

enum ELoadFlags : uint32_t
{
    LOAD_None = 0x00000000,
    LOAD_NoWarn = 0x00000010,
    LOAD_Quiet = 0x00002000,
};

// Epic's FLinkerInstancingContext (LinkerInstancingContext.h): only passed as null.
class FLinkerInstancingContext;

template <typename... ParamTypes>
class TMulticastDelegate;
using FSimpleMulticastDelegate = TMulticastDelegate<>;

// Epic's FCoreUObjectDelegates (UObjectGlobals.h): the engine's own broadcasts about UObjects, reached through the
// build's statics (found once, on the game thread).
struct FCoreUObjectDelegates
{
    // Epic's: what CollectGarbageInternal broadcasts before it marks what is reachable, so a system can let go of objects
    // that are on their way out while its references to them are still whole. Null where the build's isn't found
    // (Finder::ResolvePreGarbageCollectLive, logged once): only looked for on UE 4.21 and later builds that need it.
    static FSimpleMulticastDelegate* GetPreGarbageCollectDelegate();
};

UObject* StaticFindObject(UClass* Class, UObject* InOuter, const wchar_t* Name, bool ExactClass = false);
UObject* StaticLoadObject(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename = nullptr, uint32_t LoadFlags = LOAD_None, void* Sandbox = nullptr, bool bAllowObjectReconciliation = true, const FLinkerInstancingContext* InstancingContext = nullptr);
UObject* StaticConstructObject_Internal(UClass* Class, UObject* InOuter = nullptr, FName Name = FName(), EObjectFlags SetFlags = RF_NoFlags, EInternalObjectFlags InternalSetFlags = EInternalObjectFlags::None, UObject* Template = nullptr, bool bCopyTransientsFromClassDefaults = false, void* InstanceGraph = nullptr, bool bAssumeTemplateIsArchetype = false);

template <class T>
T* FindObject(UObject* Outer, const wchar_t* Name, bool ExactClass = false)
{
    return static_cast<T*>(StaticFindObject(T::StaticClass(), Outer, Name, ExactClass));
}

template <class T>
T* LoadObject(UObject* Outer, const wchar_t* Name, const wchar_t* Filename = nullptr, uint32_t LoadFlags = LOAD_None, void* Sandbox = nullptr)
{
    return static_cast<T*>(StaticLoadObject(T::StaticClass(), Outer, Name, Filename, LoadFlags, Sandbox));
}

template <class T>
T* NewObject(UObject* Outer, UClass* Class, FName Name = FName(), EObjectFlags Flags = RF_NoFlags, UObject* Template = nullptr, bool bCopyTransientsFromClassDefaults = false, void* InInstanceGraph = nullptr)
{
    return static_cast<T*>(StaticConstructObject_Internal(Class, Outer, Name, Flags, EInternalObjectFlags::None, Template, bCopyTransientsFromClassDefaults, InInstanceGraph));
}

template <class T>
const T* GetDefault()
{
    return static_cast<const T*>(T::StaticClass()->GetDefaultObject());
}

template <class T>
T* GetMutableDefault()
{
    return static_cast<T*>(T::StaticClass()->GetDefaultObject());
}
