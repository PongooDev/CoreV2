#include "pch.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectArray.h"
#include "Natives.h"

UObject* StaticFindObject(UClass* Class, UObject* InOuter, const wchar_t* Name, bool bExactClass)
{
    return Natives::StaticFindObject ? Natives::StaticFindObject(Class, InOuter, Name, bExactClass) : nullptr;
}

UObject* StaticLoadObject(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename, uint32_t LoadFlags)
{
    return Natives::StaticLoadObject ? Natives::StaticLoadObject(Class, InOuter, Name, Filename, LoadFlags, nullptr, true) : nullptr;
}

UClass* FindClass(const wchar_t* PathName)
{
    return static_cast<UClass*>(StaticFindObject(UClass::StaticClass(), nullptr, PathName, false));
}

FUObjectItem* GetObjectItem(const UObjectBase* Object)
{
    if (!Object || !Natives::GUObjectArray)
    {
        return nullptr;
    }

    FUObjectItem* Item = Natives::GUObjectArray->IndexToObject(Object->InternalIndex);
    return Item && Item->Object == Object ? Item : nullptr;
}
