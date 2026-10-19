#include "pch.h"
#include "UObject/UObjectGlobals.h"
#include "Natives.h"

UObject* StaticFindObject(UClass* Class, UObject* InOuter, const wchar_t* Name, bool ExactClass)
{
    return Natives::StaticFindObject(Class, InOuter, Name, ExactClass);
}

UObject* StaticLoadObject(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename, uint32_t LoadFlags, void* Sandbox, bool bAllowObjectReconciliation)
{
    return Natives::StaticLoadObject(Class, InOuter, Name, Filename, LoadFlags, Sandbox, bAllowObjectReconciliation);
}

UObject* StaticConstructObject_Internal(UClass* Class, UObject* InOuter, FName Name, EObjectFlags SetFlags, EInternalObjectFlags InternalSetFlags, UObject* Template, bool bCopyTransientsFromClassDefaults, void* InstanceGraph, bool bAssumeTemplateIsArchetype)
{
    return Natives::StaticConstructObject_Internal(Class, InOuter, Name, SetFlags, static_cast<int32_t>(InternalSetFlags), Template, bCopyTransientsFromClassDefaults, InstanceGraph, bAssumeTemplateIsArchetype);
}
