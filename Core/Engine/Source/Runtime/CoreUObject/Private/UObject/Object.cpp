#include "pch.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectGlobals.h"
#include "Log.h"
#include "Natives.h"

#include <intrin.h>
#include <mutex>
#include <unordered_set>

namespace
{
    std::mutex MissingMemberMutex;
    std::unordered_set<std::wstring> MissingMembers;

    void ReportMissingMember(const UObject* Object, const wchar_t* Name)
    {
        std::wstring Key = Object->GetClass()->GetName() + L"::" + Name;

        std::scoped_lock Lock(MissingMemberMutex);
        if (MissingMembers.insert(Key).second)
        {
            Log::Error(L"Member {} was not found", Key);
        }
    }
}

std::wstring UObject::GetPathName() const
{
    std::wstring Result;

    if (const UObject* Outer = GetOuter())
    {
        Result = Outer->GetPathName();

        const UObject* OuterOuter = Outer->GetOuter();
        const bool bOuterIsPackage = Outer->GetClass()->HasAnyCastFlag(EClassCastFlags::Package);
        const bool bOuterOuterIsPackage = OuterOuter && OuterOuter->GetClass()->HasAnyCastFlag(EClassCastFlags::Package);

        Result += !bOuterIsPackage && bOuterOuterIsPackage ? L':' : L'.';
    }

    return Result + GetName();
}

std::wstring UObject::GetFullName() const
{
    return GetClass()->GetName() + L" " + GetPathName();
}

bool UObject::IsA(const UClass* Class) const
{
    return Class && GetClass()->IsChildOf(Class);
}

void UObject::ProcessEvent(UFunction* Function, void* Parms)
{
    if (!Function || Natives::ProcessEventIndex < 0)
    {
        Log::Error(L"ProcessEvent on {} has no {}", GetName(), Function ? L"vtable index" : L"function");
        return;
    }

    using FProcessEvent = void (*)(UObject*, UFunction*, void*);
    reinterpret_cast<FProcessEvent>(VTable[Natives::ProcessEventIndex])(this, Function, Parms);
}

UFunction* UObject::FindFunction(const wchar_t* Name) const
{
    return GetClass()->FindFunctionByName(FName(Name, FNAME_Find));
}

void UObject::AddToRoot()
{
    if (FUObjectItem* Item = GetObjectItem(this))
    {
        _InterlockedOr(reinterpret_cast<volatile long*>(&Item->Flags), static_cast<long>(EInternalObjectFlags::RootSet));
    }
}

void UObject::RemoveFromRoot()
{
    if (FUObjectItem* Item = GetObjectItem(this))
    {
        _InterlockedAnd(reinterpret_cast<volatile long*>(&Item->Flags), ~static_cast<long>(EInternalObjectFlags::RootSet));
    }
}

bool UObject::IsRooted() const
{
    const FUObjectItem* Item = GetObjectItem(this);
    return Item && (Item->Flags & static_cast<int32_t>(EInternalObjectFlags::RootSet)) != 0;
}

bool UObject::IsPendingKill() const
{
    const FUObjectItem* Item = GetObjectItem(this);
    return !Item || (Item->Flags & static_cast<int32_t>(EInternalObjectFlags::PendingKill)) != 0;
}

UProperty* UObject::FindProperty(const wchar_t* Name) const
{
    return GetClass()->FindPropertyByName(FName(Name, FNAME_Find));
}

void* UObject::GetMemberAddress(const wchar_t* Name) const
{
    if (const UProperty* Property = FindProperty(Name))
    {
        return Property->ContainerPtrToValuePtr(this);
    }

    ReportMissingMember(this, Name);
    return nullptr;
}

bool UObject::GetBool(const wchar_t* Name) const
{
    const UProperty* Property = FindProperty(Name);
    if (!Property)
    {
        ReportMissingMember(this, Name);
        return false;
    }

    if (Property->GetClass()->HasAnyCastFlag(EClassCastFlags::BoolProperty))
    {
        return static_cast<const UBoolProperty*>(Property)->GetPropertyValue(this);
    }

    return *static_cast<const bool*>(Property->ContainerPtrToValuePtr(this));
}

void UObject::SetBool(const wchar_t* Name, bool bValue)
{
    const UProperty* Property = FindProperty(Name);
    if (!Property)
    {
        ReportMissingMember(this, Name);
        return;
    }

    if (Property->GetClass()->HasAnyCastFlag(EClassCastFlags::BoolProperty))
    {
        static_cast<const UBoolProperty*>(Property)->SetPropertyValue(this, bValue);
        return;
    }

    *static_cast<bool*>(Property->ContainerPtrToValuePtr(this)) = bValue;
}
