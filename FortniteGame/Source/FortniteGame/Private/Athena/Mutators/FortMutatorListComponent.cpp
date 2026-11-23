#include "pch.h"
#include "Athena/Mutators/FortMutatorListComponent.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UObject* UFortMutatorListComponent::GetMutatorByClass(UClass* MutatorClass) const
{
    Reflection::FParams Params(FindStaticFunction_GetMutatorByClass());
    if (!Params.Set(L"MutatorClass", MutatorClass) || !Params.Call(const_cast<UFortMutatorListComponent*>(this)))
    {
        return nullptr;
    }
    return Params.Take<UObject*>(L"ReturnValue");
}
