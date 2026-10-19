#include "pch.h"
#include "Reflection.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"
#include "Log.h"

#include <vector>

namespace
{
    std::vector<Reflection::FTypeInfo*>& Types()
    {
        static std::vector<Reflection::FTypeInfo*> Registered;
        return Registered;
    }

    std::vector<Reflection::FMemberInfo*>& Members()
    {
        static std::vector<Reflection::FMemberInfo*> Registered;
        return Registered;
    }

    std::vector<Reflection::FFunctionInfo*>& Functions()
    {
        static std::vector<Reflection::FFunctionInfo*> Registered;
        return Registered;
    }

    std::wstring OwnerName(Reflection::FOwnerGetter Owner)
    {
        const UStruct* Struct = Owner();
        return Struct ? Struct->GetName().ToStdString() : L"<missing type>";
    }
}

Reflection::FTypeInfo::FTypeInfo(const wchar_t* InPackage, const wchar_t* InNativeName, ETypeKind InKind)
    : Package(InPackage), NativeName(InNativeName), Kind(InKind)
{
    Types().push_back(this);
}

UStruct* Reflection::FTypeInfo::Get()
{
    if (!Struct)
    {
        const std::wstring Path = std::format(L"/Script/{}.{}", Package, NativeName + 1);
        UObject* Found = StaticFindObject(nullptr, nullptr, Path.c_str(), false);
        const EClassCastFlags Expected = Kind == ETypeKind::Class ? CASTCLASS_UClass : CASTCLASS_UScriptStruct;

        if (Found && Found->GetClass()->HasAnyCastFlag(Expected))
        {
            Struct = static_cast<UStruct*>(Found);
        }
    }

    return Struct;
}

UClass* Reflection::FTypeInfo::GetClass()
{
    return Kind == ETypeKind::Class ? static_cast<UClass*>(Get()) : nullptr;
}

UScriptStruct* Reflection::FTypeInfo::GetScriptStruct()
{
    return Kind == ETypeKind::ScriptStruct ? static_cast<UScriptStruct*>(Get()) : nullptr;
}

Reflection::FMemberInfo::FMemberInfo(FOwnerGetter InOwner, const wchar_t* InName, size_t InSize, EMemberKind InKind)
    : Owner(InOwner), Name(InName), Size(InSize), Kind(InKind)
{
    Members().push_back(this);
}

bool Reflection::FMemberInfo::Resolve()
{
    const UStruct* Struct = Owner();
    if (!Struct)
    {
        return false;
    }

    const UProperty* Property = Struct->FindPropertyByName(FName(Name, FNAME_Find));
    if (!Property)
    {
        Offset = MissingOffset;
        return false;
    }

    if (Kind == EMemberKind::Bool)
    {
        if (!Property->GetClass()->HasAnyCastFlag(CASTCLASS_UBoolProperty))
        {
            Offset = MissingOffset;
            return false;
        }

        const UBoolProperty* BoolProperty = static_cast<const UBoolProperty*>(Property);
        Offset = Property->GetOffset_ForInternal() + BoolProperty->ByteOffset;
        FieldMask = BoolProperty->FieldMask;
        return true;
    }

    if (static_cast<size_t>(Property->ElementSize) != Size)
    {
        Offset = MissingOffset;
        return false;
    }

    Offset = Property->GetOffset_ForInternal();
    return true;
}

int32_t Reflection::FMemberInfo::GetOffsetChecked()
{
    if (Offset == UnresolvedOffset)
    {
        Resolve();
    }

    if (Offset < 0)
    {
        Log::Fatal(L"Member {}::{} is not available", OwnerName(Owner), Name);
    }

    return Offset;
}

Reflection::FFunctionInfo::FFunctionInfo(FOwnerGetter InOwner, const wchar_t* InName)
    : Owner(InOwner), Name(InName)
{
    Functions().push_back(this);
}

UFunction* Reflection::FFunctionInfo::Get()
{
    if (!Function)
    {
        if (const UStruct* Struct = Owner(); Struct && Struct->GetClass()->HasAnyCastFlag(CASTCLASS_UClass))
        {
            Function = static_cast<const UClass*>(Struct)->FindFunctionByName(FName(Name, FNAME_Find));
        }

        if (!Function)
        {
            Log::Fatal(L"Function {}::{} is not available", OwnerName(Owner), Name);
        }
    }

    return Function;
}

bool Reflection::ResolveAll()
{
    int32_t Missing = 0;

    for (FTypeInfo* Type : Types())
    {
        if (!Type->Get())
        {
            Log::Error(L"Type /Script/{}.{} was not found", Type->GetPackage(), Type->GetNativeName() + 1);
            ++Missing;
        }
    }

    for (FMemberInfo* Member : Members())
    {
        if (Member->Owner() && !Member->Resolve())
        {
            Log::Error(L"Member {}::{} was not found or has a different type", OwnerName(Member->Owner), Member->Name);
            ++Missing;
        }
    }

    for (FFunctionInfo* Function : Functions())
    {
        const UStruct* Struct = Function->Owner();
        if (Struct && Struct->GetClass()->HasAnyCastFlag(CASTCLASS_UClass))
        {
            Function->Function = static_cast<const UClass*>(Struct)->FindFunctionByName(FName(Function->Name, FNAME_Find));
        }

        if (Struct && !Function->Function)
        {
            Log::Error(L"Function {}::{} was not found", OwnerName(Function->Owner), Function->Name);
            ++Missing;
        }
    }

    Log::Info(L"Reflection: {} types, {} members, {} functions, {} missing", Types().size(), Members().size(), Functions().size(), Missing);
    return Missing == 0;
}
