#pragma once

#include "ObjectLayout.h"
#include "UObject/Class.h"
#include "UObject/ScriptInterface.h"

#include <cstdint>

class UBoolProperty;
class UByteProperty;
class UFloatProperty;
class UIntProperty;
class UInterfaceProperty;
class UObject;
class UProperty;
class UStructProperty;

template <class PropertyType>
struct TPropertyCppType;

template <>
struct TPropertyCppType<UIntProperty>
{
    using Type = int32_t;
};

template <>
struct TPropertyCppType<UByteProperty>
{
    using Type = uint8_t;
};

template <>
struct TPropertyCppType<UFloatProperty>
{
    using Type = float;
};

#define RESULT_PARAM Z_Param__Result
#define RESULT_DECL void* const RESULT_PARAM

struct FFrame
{
    DefineNativeMember(UObject*, Object, FFrame_Object)
    DefineNativeMember(uint8_t*, Code, FFrame_Code)
    DefineNativeMember(UField*, PropertyChainForCompiledIn, FFrame_PropertyChainForCompiledIn)

public:
    void Step(UObject* Context, RESULT_DECL);
    void StepExplicitProperty(void* const Result, UProperty* Property);

    template <class TProperty>
    void StepCompiledIn(void* const Result)
    {
        if (Code)
        {
            Step(Object, Result);
        }
        else
        {
            UField* Property = PropertyChainForCompiledIn;
            PropertyChainForCompiledIn = Property->Next;
            StepExplicitProperty(Result, reinterpret_cast<UProperty*>(Property));
        }
    }
};

#define DECLARE_FUNCTION(func) static void func(UObject* Context, void* TheStack, RESULT_DECL)
#define DEFINE_FUNCTION(func) void func(UObject* Context, void* TheStack, RESULT_DECL)
#define P_STACK (*static_cast<FFrame*>(TheStack))
#define P_GET_OBJECT(ObjectType, ParamName) ObjectType* ParamName = nullptr; P_STACK.StepCompiledIn<UObjectPropertyBase>(&ParamName);
#define P_GET_PROPERTY(PropertyType, ParamName) typename TPropertyCppType<PropertyType>::Type ParamName{}; P_STACK.StepCompiledIn<PropertyType>(&ParamName);
#define P_GET_STRUCT(StructType, ParamName) StructType ParamName{}; P_STACK.StepCompiledIn<UStructProperty>(&ParamName);
#define P_GET_UBOOL(ParamName) uint32_t ParamName##32 = 0; P_STACK.StepCompiledIn<UBoolProperty>(&ParamName##32); bool ParamName = !!ParamName##32;
#define P_GET_TINTERFACE(InterfaceType, ParamName) TScriptInterface<InterfaceType> ParamName; P_STACK.StepCompiledIn<UInterfaceProperty>(&ParamName);
#define P_FINISH P_STACK.Code += !!P_STACK.Code;
#define P_NATIVE_BEGIN
#define P_NATIVE_END
