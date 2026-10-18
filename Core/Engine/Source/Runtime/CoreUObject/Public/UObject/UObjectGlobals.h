#pragma once

#include "UObject/Class.h"

#define ANY_PACKAGE (reinterpret_cast<UObject*>(-1))

enum ELoadFlags : uint32_t
{
    LOAD_None = 0x00000000,
    LOAD_NoWarn = 0x00000010,
    LOAD_Quiet = 0x00002000,
};

UObject* StaticFindObject(UClass* Class, UObject* InOuter, const wchar_t* Name, bool ExactClass = false);
UObject* StaticLoadObject(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename = nullptr, uint32_t LoadFlags = LOAD_None, void* Sandbox = nullptr, bool bAllowObjectReconciliation = true);

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
const T* GetDefault()
{
    return static_cast<const T*>(T::StaticClass()->GetDefaultObject());
}

template <class T>
T* GetMutableDefault()
{
    return static_cast<T*>(T::StaticClass()->GetDefaultObject());
}
