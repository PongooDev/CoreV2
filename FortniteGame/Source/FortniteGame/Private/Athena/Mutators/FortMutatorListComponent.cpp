#include "pch.h"
#include "Athena/Mutators/FortMutatorListComponent.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

UObject* UFortMutatorListComponent::GetMutatorByClass(UClass* MutatorClass) const
{
    if (!FindStaticFunction_GetMutatorByClass())
    {
        // 8.00 (CL-5203069) has no GetMutatorByClass on the list; 8.20 adds it as a lookup in Mutators.
        if (!MutatorClass || !HasMutators())
        {
            return nullptr;
        }
        for (UObject* const Mutator : Mutators)
        {
            if (Mutator && Mutator->IsA(MutatorClass))
            {
                return Mutator;
            }
        }
        return nullptr;
    }
    Reflection::FParams Params(FindStaticFunction_GetMutatorByClass());
    if (!Params.Set(L"MutatorClass", MutatorClass) || !Params.Call(const_cast<UFortMutatorListComponent*>(this)))
    {
        return nullptr;
    }
    return Params.Take<UObject*>(L"ReturnValue");
}
