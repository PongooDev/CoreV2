#include "pch.h"
#include "UObject/Class.h"
#include "Finder.h"
#include "Log.h"
#include "Natives.h"

void UObject::ProcessEvent(UFunction* Function, void* Parms)
{
    if (!Function)
    {
        Log::Error(L"ProcessEvent on {} with no function", *GetName());
        return;
    }

    if (Natives::ProcessEventIndex < 0 && !Finder::ResolveProcessEvent())
    {
        return;
    }

    using FProcessEvent = void (*)(UObject*, UFunction*, void*);
    void** VTable = *reinterpret_cast<void***>(this);
    reinterpret_cast<FProcessEvent>(VTable[Natives::ProcessEventIndex])(this, Function, Parms);
}

UFunction* UObject::FindFunction(FName InName) const
{
    return GetClass()->FindFunctionByName(InName);
}

UFunction* UObject::FindFunctionChecked(FName InName) const
{
    UFunction* Result = FindFunction(InName);
    if (!Result)
    {
        Log::Fatal(L"Failed to find function {} in {}", *InName.ToString(), *GetFullName());
    }

    return Result;
}

UObject* UObject::GetArchetypeFromRequiredInfo(UClass* Class, UObject* Outer, FName Name, EObjectFlags ObjectFlags)
{
    return Natives::UObject_GetArchetypeFromRequiredInfo(Class, Outer, Name, static_cast<int32_t>(ObjectFlags));
}
