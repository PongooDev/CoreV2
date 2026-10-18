#pragma once

#include "UObject/Object.h"
#include "UObject/ObjectLayout.h"

class UProperty;

class UField : public UObject
{
public:
    UField* GetNext() const
    {
        return *reinterpret_cast<UField* const*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UField_Next);
    }
};

class UStruct : public UField
{
public:
    UStruct* GetSuperStruct() const
    {
        return *reinterpret_cast<UStruct* const*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UStruct_SuperStruct);
    }

    UField* GetChildren() const
    {
        return *reinterpret_cast<UField* const*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UStruct_Children);
    }

    int32_t GetPropertiesSize() const
    {
        return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UStruct_PropertiesSize);
    }

    bool IsChildOf(const UStruct* Base) const;
    UProperty* FindPropertyByName(FName Name) const;
    UFunction* FindFunctionByName(FName Name) const;
};

class UClass : public UStruct
{
public:
    EClassCastFlags GetClassCastFlags() const
    {
        return *reinterpret_cast<const EClassCastFlags*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UClass_ClassCastFlags);
    }

    bool HasAnyCastFlag(EClassCastFlags Flags) const
    {
        return HasAnyCastFlags(GetClassCastFlags(), Flags);
    }

    UObject* GetDefaultObject() const
    {
        return *reinterpret_cast<UObject* const*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UClass_ClassDefaultObject);
    }

    static UClass* StaticClass();
};

class UFunction : public UStruct
{
public:
    using FNativeFuncPtr = void (*)(UObject* Context, void* Stack, void* Result);

    uint32_t GetFunctionFlags() const
    {
        return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UFunction_FunctionFlags);
    }

    uint16_t GetParmsSize() const
    {
        return *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UFunction_ParmsSize);
    }

    uint16_t GetReturnValueOffset() const
    {
        return *reinterpret_cast<const uint16_t*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UFunction_ReturnValueOffset);
    }

    FNativeFuncPtr GetNativeFunc() const
    {
        return *reinterpret_cast<const FNativeFuncPtr*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UFunction_Func);
    }
};
