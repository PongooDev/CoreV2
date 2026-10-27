#include "pch.h"
#include "Components/PrimitiveComponent.h"
#include "ReflectionParams.h"

int32_t UPrimitiveComponent::GetNumMaterials() const
{
    Reflection::FParams Params(StaticFunction_GetNumMaterials());
    return Params.Call(const_cast<UPrimitiveComponent*>(this)) ? Params.Take<int32_t>(L"ReturnValue") : 0;
}

UMaterialInterface* UPrimitiveComponent::GetMaterial(int32_t ElementIndex) const
{
    Reflection::FParams Params(StaticFunction_GetMaterial());
    Params.Set(L"ElementIndex", ElementIndex);
    return Params.Call(const_cast<UPrimitiveComponent*>(this)) ? Params.Take<UMaterialInterface*>(L"ReturnValue") : nullptr;
}

void UPrimitiveComponent::SetMaterial(int32_t ElementIndex, UMaterialInterface* Material)
{
    Reflection::FParams Params(StaticFunction_SetMaterial());
    Params.Set(L"ElementIndex", ElementIndex);
    Params.Set(L"Material", Material);
    Params.Call(this);
}
