#pragma once

#include "UObject/Class.h"

struct FUObjectItem;

UObject* StaticFindObject(UClass* Class, UObject* InOuter, const wchar_t* Name, bool bExactClass = false);
UObject* StaticLoadObject(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename = nullptr, uint32_t LoadFlags = 0);
UClass* FindClass(const wchar_t* PathName);

template <typename T>
T* FindObject(const wchar_t* PathName, UClass* Class = nullptr)
{
    return static_cast<T*>(StaticFindObject(Class, nullptr, PathName, false));
}

template <typename T>
T* LoadObject(const wchar_t* PathName, UClass* Class = nullptr)
{
    return static_cast<T*>(StaticLoadObject(Class, nullptr, PathName));
}

FUObjectItem* GetObjectItem(const UObjectBase* Object);
