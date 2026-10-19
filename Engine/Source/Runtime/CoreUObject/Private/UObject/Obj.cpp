#include "pch.h"
#include "UObject/Class.h"
#include "Finder.h"
#include "Hooks.h"
#include "Misc/AssertionMacros.h"
#include "Misc/CoreMisc.h"
#include "ObjectLayout.h"
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

UWorld* UObject::GetWorld() const
{
    return CallVirtual<UWorld*>(this, Natives::UObject_GetWorldIndex);
}

bool UObject::NeedsLoadForClient() const
{
    return CallVirtual<bool>(this, Natives::UObject_NeedsLoadForClientIndex);
}

bool UObject::NeedsLoadForServer() const
{
    return CallVirtual<bool>(this, Natives::UObject_NeedsLoadForServerIndex);
}

bool UObject::CanCreateInCurrentContext(UObject* Template)
{
    check(Template);

    if (IsRunningDedicatedServer())
    {
        return Template->NeedsLoadForServer();
    }

    if (IsRunningClientOnly())
    {
        return Template->NeedsLoadForClient();
    }

    if (IsRunningGame())
    {
        return Template->NeedsLoadForClient() || Template->NeedsLoadForServer();
    }

    return true;
}

void UObject::Hook()
{
    Hooks::Detour(L"UObject::CanCreateInCurrentContext", reinterpret_cast<void*>(Natives::UObject_CanCreateInCurrentContext), &CanCreateInCurrentContext, nullptr);
}
