#pragma once

#include "UObject/Class.h"

class UProperty : public UField
{
    DefineUnrealClass(UProperty, CoreUObject)
    DefineLayoutMember(int32_t, ArrayDim, UProperty_ArrayDim)
    DefineLayoutMember(int32_t, ElementSize, UProperty_ElementSize)
    DefineLayoutMember(uint64_t, PropertyFlags, UProperty_PropertyFlags)

public:
    int32_t GetOffset_ForInternal() const
    {
        return *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(this) + ObjectLayout::Current.UProperty_Offset_Internal);
    }

    int32_t GetSize() const { return ArrayDim * ElementSize; }

    template <typename ValueType>
    ValueType* ContainerPtrToValuePtr(const void* ContainerPtr, int32_t ArrayIndex = 0) const
    {
        return reinterpret_cast<ValueType*>(reinterpret_cast<uintptr_t>(ContainerPtr) + GetOffset_ForInternal() + ElementSize * ArrayIndex);
    }
};

class UBoolProperty : public UProperty
{
    DefineUnrealClass(UBoolProperty, CoreUObject)
    DefineLayoutMember(uint8_t, FieldSize, UBoolProperty_FieldSize)
    DefineLayoutMember(uint8_t, ByteOffset, UBoolProperty_ByteOffset)
    DefineLayoutMember(uint8_t, ByteMask, UBoolProperty_ByteMask)
    DefineLayoutMember(uint8_t, FieldMask, UBoolProperty_FieldMask)

public:
    bool GetPropertyValue(const void* A) const
    {
        return (*(static_cast<const uint8_t*>(A) + ByteOffset) & FieldMask) != 0;
    }

    void SetPropertyValue(void* A, bool Value) const
    {
        uint8_t* ByteValue = static_cast<uint8_t*>(A) + ByteOffset;
        *ByteValue = static_cast<uint8_t>((*ByteValue & ~FieldMask) | (Value ? ByteMask : 0));
    }

    bool GetPropertyValue_InContainer(const void* A, int32_t ArrayIndex = 0) const
    {
        return GetPropertyValue(ContainerPtrToValuePtr<void>(A, ArrayIndex));
    }

    void SetPropertyValue_InContainer(void* A, bool Value, int32_t ArrayIndex = 0) const
    {
        SetPropertyValue(ContainerPtrToValuePtr<void>(A, ArrayIndex), Value);
    }
};

class UObjectPropertyBase : public UProperty
{
    DefineUnrealClass(UObjectPropertyBase, CoreUObject)
    DefineLayoutMember(UClass*, PropertyClass, UObjectPropertyBase_PropertyClass)
};

class UStructProperty : public UProperty
{
    DefineUnrealClass(UStructProperty, CoreUObject)
    DefineLayoutMember(UScriptStruct*, Struct, UStructProperty_Struct)
};

class UArrayProperty : public UProperty
{
    DefineUnrealClass(UArrayProperty, CoreUObject)
    DefineLayoutMember(UProperty*, Inner, UArrayProperty_Inner)
};
