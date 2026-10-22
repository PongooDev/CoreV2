#pragma once

#include "ObjectLayout.h"
#include "UObject/Object.h"

class UProperty;

namespace EIncludeSuperFlag
{
    enum Type
    {
        ExcludeSuper,
        IncludeSuper,
    };
}

class UField : public UObject
{
    DefineUnrealClass(UField, CoreUObject)
    DefineLayoutMember(UField*, Next, UField_Next)
};

class UStruct : public UField
{
    DefineUnrealClass(UStruct, CoreUObject)
    DefineLayoutMember(UField*, Children, UStruct_Children)
    DefineLayoutMember(int32_t, PropertiesSize, UStruct_PropertiesSize)
    DefineLayoutMember(int32_t, MinAlignment, UStruct_MinAlignment)

public:
    UStruct* GetSuperStruct() const
    {
        return *reinterpret_cast<UStruct* const*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.UStruct_SuperStruct);
    }

    int32_t GetPropertiesSize() const { return PropertiesSize; }
    int32_t GetMinAlignment() const { return MinAlignment; }

    bool IsChildOf(const UStruct* SomeBase) const;
    UProperty* FindPropertyByName(FName InName) const;

    void InitializeStruct(void* Dest, int32_t ArrayDim = 1) const;
    void DestroyStruct(void* Dest, int32_t ArrayDim = 1) const;
};

class UScriptStruct : public UStruct
{
    DefineUnrealClass(UScriptStruct, CoreUObject)

public:
    int32_t GetStructureSize() const;
    void CopyScriptStruct(void* Dest, void const* Src, int32_t ArrayDim = 1) const;
};

class UClass : public UStruct
{
    DefineUnrealClass(UClass, CoreUObject)
    DefineLayoutMember(EClassCastFlags, ClassCastFlags, UClass_ClassCastFlags)
    DefineLayoutMember(UObject*, ClassDefaultObject, UClass_ClassDefaultObject)

public:
    UClass* GetSuperClass() const { return static_cast<UClass*>(GetSuperStruct()); }
    bool HasAnyCastFlag(EClassCastFlags FlagToCheck) const { return (ClassCastFlags & FlagToCheck) != 0; }
    bool HasAllCastFlags(EClassCastFlags FlagsToCheck) const { return (ClassCastFlags & FlagsToCheck) == FlagsToCheck; }

    UObject* GetDefaultObject(bool bCreateIfNeeded = true) const
    {
        if (ClassDefaultObject == nullptr && bCreateIfNeeded)
        {
            const_cast<UClass*>(this)->CreateDefaultObject();
        }

        return ClassDefaultObject;
    }

    UObject* CreateDefaultObject();

    UFunction* FindFunctionByName(FName InName, EIncludeSuperFlag::Type IncludeSuper = EIncludeSuperFlag::IncludeSuper) const;
};

class UFunction : public UStruct
{
    DefineUnrealClass(UFunction, CoreUObject)
    DefineLayoutMember(EFunctionFlags, FunctionFlags, UFunction_FunctionFlags)
    DefineLayoutMember(uint16_t, ParmsSize, UFunction_ParmsSize)
    DefineLayoutMember(uint16_t, ReturnValueOffset, UFunction_ReturnValueOffset)

public:
    using FNativeFuncPtr = void (*)(UObject* Context, void* TheStack, void* Result);

    FNativeFuncPtr GetNativeFunc() const
    {
        return *reinterpret_cast<const FNativeFuncPtr*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.UFunction_Func);
    }

    void SetNativeFunc(FNativeFuncPtr InFunc)
    {
        *reinterpret_cast<FNativeFuncPtr*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.UFunction_Func) = InFunc;
    }

    bool HasAnyFunctionFlags(EFunctionFlags FlagsToCheck) const { return (FunctionFlags & FlagsToCheck) != 0; }

    UFunction* GetSuperFunction() const { return static_cast<UFunction*>(GetSuperStruct()); }
};

class UEnum : public UField
{
    DefineUnrealClass(UEnum, CoreUObject)
};
