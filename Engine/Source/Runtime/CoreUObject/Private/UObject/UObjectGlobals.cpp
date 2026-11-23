#include "pch.h"
#include "UObject/UObjectGlobals.h"
#include "Delegates/Delegate.h"
#include "Finder.h"
#include "Natives.h"

FSimpleMulticastDelegate* FCoreUObjectDelegates::GetPreGarbageCollectDelegate()
{
    return Finder::ResolvePreGarbageCollectLive() ? static_cast<FSimpleMulticastDelegate*>(Natives::FCoreUObjectDelegates_PreGarbageCollect) : nullptr;
}

UObject* StaticFindObject(UClass* Class, UObject* InOuter, const wchar_t* Name, bool ExactClass)
{
    return Natives::StaticFindObject(Class, InOuter, Name, ExactClass);
}

UObject* StaticLoadObject(UClass* Class, UObject* InOuter, const wchar_t* Name, const wchar_t* Filename, uint32_t LoadFlags, void* Sandbox, bool bAllowObjectReconciliation, const FLinkerInstancingContext* InstancingContext)
{
    // 8.30 (CL-5793395) added InstancingContext and passes it on to StaticLoadObjectInternal; older builds ignore the
    // extra argument.
    return Natives::StaticLoadObject(Class, InOuter, Name, Filename, LoadFlags, Sandbox, bAllowObjectReconciliation, InstancingContext);
}

UObject* StaticConstructObject_Internal(UClass* Class, UObject* InOuter, FName Name, EObjectFlags SetFlags, EInternalObjectFlags InternalSetFlags, UObject* Template, bool bCopyTransientsFromClassDefaults, void* InstanceGraph, bool bAssumeTemplateIsArchetype)
{
    return Natives::StaticConstructObject_Internal(Class, InOuter, Name, SetFlags, static_cast<int32_t>(InternalSetFlags), Template, bCopyTransientsFromClassDefaults, InstanceGraph, bAssumeTemplateIsArchetype);
}
