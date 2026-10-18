#pragma once

#include <cstdint>

struct FObjectLayout
{
    int32_t UField_Next = 0x28;
    int32_t UStruct_SuperStruct = 0x30;
    int32_t UStruct_Children = 0x38;
    int32_t UStruct_PropertiesSize = 0x40;
    int32_t UClass_ClassCastFlags = 0xB8;
    int32_t UClass_ClassDefaultObject = 0x100;
    int32_t UFunction_FunctionFlags = 0x88;
    int32_t UFunction_ParmsSize = 0x90;
    int32_t UFunction_ReturnValueOffset = 0x92;
    int32_t UFunction_Func = 0xB0;
    int32_t UProperty_ArrayDim = 0x30;
    int32_t UProperty_ElementSize = 0x34;
    int32_t UProperty_PropertyFlags = 0x38;
    int32_t UProperty_Offset = 0x50;
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
