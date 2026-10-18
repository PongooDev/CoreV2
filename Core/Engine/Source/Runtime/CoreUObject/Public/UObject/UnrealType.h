#pragma once

#include "UObject/Class.h"

class UProperty : public UField
{
public:
    int32_t GetArrayDim() const
    {
        return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UProperty_ArrayDim);
    }

    int32_t GetElementSize() const
    {
        return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UProperty_ElementSize);
    }

    uint64_t GetPropertyFlags() const
    {
        return *reinterpret_cast<const uint64_t*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UProperty_PropertyFlags);
    }

    int32_t GetOffset() const
    {
        return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UProperty_Offset);
    }

    void* ContainerPtrToValuePtr(const void* Container) const
    {
        return const_cast<uint8_t*>(static_cast<const uint8_t*>(Container) + GetOffset());
    }
};

class UBoolProperty : public UProperty
{
public:
    uint8_t GetByteOffset() const
    {
        return *(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UBoolProperty_ByteOffset);
    }

    uint8_t GetFieldMask() const
    {
        return *(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UBoolProperty_FieldMask);
    }

    bool GetPropertyValue(const void* Container) const
    {
        const uint8_t* Byte = static_cast<const uint8_t*>(ContainerPtrToValuePtr(Container)) + GetByteOffset();
        return (*Byte & GetFieldMask()) != 0;
    }

    void SetPropertyValue(void* Container, bool bValue) const
    {
        uint8_t* Byte = static_cast<uint8_t*>(ContainerPtrToValuePtr(Container)) + GetByteOffset();
        *Byte = bValue ? (*Byte | GetFieldMask()) : (*Byte & ~GetFieldMask());
    }
};

class UObjectPropertyBase : public UProperty
{
public:
    UClass* GetPropertyClass() const
    {
        return *reinterpret_cast<UClass* const*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UObjectPropertyBase_PropertyClass);
    }
};

class UStructProperty : public UProperty
{
public:
    UStruct* GetStruct() const
    {
        return *reinterpret_cast<UStruct* const*>(reinterpret_cast<const uint8_t*>(this) + ObjectLayout::Current.UStructProperty_Struct);
    }
};

template <typename T>
T* UObject::TryGet(const wchar_t* Name) const
{
    const UProperty* Property = FindProperty(Name);
    return Property ? static_cast<T*>(Property->ContainerPtrToValuePtr(this)) : nullptr;
}
