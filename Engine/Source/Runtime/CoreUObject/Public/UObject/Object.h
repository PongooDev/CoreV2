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

    bool NeedsLoadForClient() const;
    bool NeedsLoadForServer() const;

    static bool CanCreateInCurrentContext(UObject* Template);

    static UObject* GetArchetypeFromRequiredInfo(UClass* Class, UObject* Outer, FName Name, EObjectFlags ObjectFlags);

    UObject* GetArchetype() const
    {
        return GetArchetypeFromRequiredInfo(GetClass(), GetOuter(), GetFName(), GetFlags());
    }

    static void Hook();
};
