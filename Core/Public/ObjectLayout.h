#pragma once

#include "Natives.h"

#include <cstdint>

struct FObjectLayout
{
    int32_t UField_Next = 0x28;
    int32_t UStruct_SuperStruct = 0x30;
    int32_t UStruct_Children = 0x38;
    int32_t UStruct_PropertiesSize = 0x40;
    int32_t UStruct_MinAlignment = 0x44;
    int32_t UClass_ClassCastFlags = 0xB8;
    int32_t UClass_ClassDefaultObject = 0x100;
    int32_t UFunction_FunctionFlags = 0x88;
    int32_t UFunction_ParmsSize = 0x90;
    int32_t UFunction_ReturnValueOffset = 0x92;
    int32_t UFunction_Func = 0xB0;
    int32_t UProperty_ArrayDim = 0x30;
    int32_t UProperty_ElementSize = 0x34;
    int32_t UProperty_PropertyFlags = 0x38;
    int32_t UProperty_Offset_Internal = 0x50;
    int32_t UBoolProperty_FieldSize = 0x78;
    int32_t UBoolProperty_ByteOffset = 0x79;
    int32_t UBoolProperty_ByteMask = 0x7A;
    int32_t UBoolProperty_FieldMask = 0x7B;
    int32_t UObjectPropertyBase_PropertyClass = 0x78;
    int32_t UStructProperty_Struct = 0x78;
    int32_t UArrayProperty_Inner = 0x78;
};

namespace ObjectLayout
{
    inline FObjectLayout Current;
}

#define DefineLayoutMember(Type, Name, LayoutField) \
public: \
    Type& GetLayout_##Name() const { return *reinterpret_cast<Type*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.LayoutField); } \
    void SetLayout_##Name(Type const& Value) { GetLayout_##Name() = Value; } \
    __declspec(property(get = GetLayout_##Name, put = SetLayout_##Name)) Type Name;

#define DefineNativeMember(Type, Name, Offset) \
public: \
    Type& GetNative_##Name() const { return *reinterpret_cast<Type*>(reinterpret_cast<uintptr_t>(this) + Natives::Offset); } \
    void SetNative_##Name(Type const& Value) { GetNative_##Name() = Value; } \
    __declspec(property(get = GetNative_##Name, put = SetNative_##Name)) Type Name;

#define DefineNativeObject(Type, Name, Offset) \
public: \
    Type& GetNative_##Name() const { return *reinterpret_cast<Type*>(reinterpret_cast<uintptr_t>(this) + Natives::Offset); } \
    __declspec(property(get = GetNative_##Name)) Type Name;

#define DefineNativeArray(Type, Name, Offset) \
public: \
    Type* GetNative_##Name() const { return reinterpret_cast<Type*>(reinterpret_cast<uintptr_t>(this) + Natives::Offset); } \
    __declspec(property(get = GetNative_##Name)) Type* Name;

#define DefineNativeBitfield(Name, Offset, Mask) \
public: \
    bool GetNative_##Name() const { return (*reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(this) + Natives::Offset) & (Mask)) != 0; } \
    void SetNative_##Name(bool bValue) { uint32_t& Bits = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(this) + Natives::Offset); Bits = bValue ? (Bits | (Mask)) : (Bits & ~(Mask)); } \
    __declspec(property(get = GetNative_##Name, put = SetNative_##Name)) bool Name;

template <typename ReturnType, typename ObjectType, typename... ArgTypes>
ReturnType CallVirtual(ObjectType* Object, int32_t Index, ArgTypes... Args)
{
    using FFunction = ReturnType (*)(ObjectType*, ArgTypes...);
    return reinterpret_cast<FFunction>((*reinterpret_cast<void* const* const*>(Object))[Index])(Object, Args...);
}
