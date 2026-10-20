#pragma once

#include "ObjectLayout.h"
#include "UObject/Class.h"

class UObject;
class UProperty;

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
#define P_FINISH P_STACK.Code += !!P_STACK.Code;
#define P_NATIVE_BEGIN
#define P_NATIVE_END
