#include "pch.h"
#include "Components/SceneComponent.h"
#include "ReflectionParams.h"

void USceneComponent::SetMobility(EComponentMobility::Type NewMobility)
{
    Reflection::FParams Params(StaticFunction_SetMobility());
    Params.Set(L"NewMobility", static_cast<uint8_t>(NewMobility));
    Params.Call(this);
}
