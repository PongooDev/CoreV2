#pragma once

#include "UObject/NameTypes.h"
#include "UObject/ObjectMacros.h"

#include <string>

class UClass;
class UFunction;
class UObject;
class UProperty;

class UObjectBase
{
public:
    void** VTable;
    EObjectFlags ObjectFlags;
    int32_t InternalIndex;
    UClass* ClassPrivate;
    FName NamePrivate;
    UObject* OuterPrivate;
};
static_assert(sizeof(UObjectBase) == 0x28);

class UObject : public UObjectBase
{
public:
    UClass* GetClass() const { return ClassPrivate; }
    UObject* GetOuter() const { return OuterPrivate; }
    FName GetFName() const { return NamePrivate; }
    std::wstring GetName() const { return NamePrivate.ToStdString(); }
    std::wstring GetPathName() const;
    std::wstring GetFullName() const;

    bool IsA(const UClass* Class) const;
    bool IsDefaultObject() const { return (ObjectFlags & RF_ClassDefaultObject) != 0; }

    void ProcessEvent(UFunction* Function, void* Parms);
    UFunction* FindFunction(const wchar_t* Name) const;

    void AddToRoot();
    void RemoveFromRoot();
    bool IsRooted() const;
    bool IsPendingKill() const;

    UProperty* FindProperty(const wchar_t* Name) const;
    void* GetMemberAddress(const wchar_t* Name) const;
    bool GetBool(const wchar_t* Name) const;
    void SetBool(const wchar_t* Name, bool bValue);

    template <typename T>
    T& Get(const wchar_t* Name)
    {
        return *static_cast<T*>(GetMemberAddress(Name));
    }

    template <typename T>
    const T& Get(const wchar_t* Name) const
    {
        return *static_cast<const T*>(GetMemberAddress(Name));
    }

    template <typename T>
    T* TryGet(const wchar_t* Name) const;
};
