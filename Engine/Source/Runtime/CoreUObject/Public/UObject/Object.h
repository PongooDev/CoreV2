#pragma once

#include "Reflection.h"
#include "UObject/UObjectBaseUtility.h"

class UFunction;
class UStruct;

class UObject : public UObjectBaseUtility
{
    DefineUnrealClass(UObject, CoreUObject)

public:
    void ProcessEvent(UFunction* Function, void* Parms);
    UFunction* FindFunction(FName InName) const;
    UFunction* FindFunctionChecked(FName InName) const;
};
