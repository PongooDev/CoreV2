#pragma once

#include "Containers/Array.h"
#include "UObject/NameTypes.h"
#include "UObject/WeakObjectPtr.h"

class UObject;

struct FScriptDelegate
{
    FScriptDelegate() = default;

    FScriptDelegate(const UObject* InObject, FName InFunctionName)
        : Object(InObject), FunctionName(InFunctionName)
    {
    }

    bool operator==(const FScriptDelegate& Other) const
    {
        return Object == Other.Object && FunctionName == Other.FunctionName;
    }

private:
    FWeakObjectPtr Object;
    FName FunctionName;
};

static_assert(sizeof(FScriptDelegate) == 0x10);

struct FMulticastScriptDelegate
{
    bool Contains(const FScriptDelegate& InDelegate) const
    {
        for (int32_t Index = 0; Index < InvocationList.Num(); ++Index)
        {
            if (InvocationList[Index] == InDelegate)
            {
                return true;
            }
        }
        return false;
    }

    void AddUnique(const FScriptDelegate& InDelegate)
    {
        if (!Contains(InDelegate))
        {
            InvocationList.Add(InDelegate);
        }
    }

private:
    TArray<FScriptDelegate> InvocationList;
};

static_assert(sizeof(FMulticastScriptDelegate) == 0x10);
