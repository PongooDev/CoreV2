#include "pch.h"
#include "BlueprintGameplayTagLibrary.h"
#include "ReflectionParams.h"

void UBlueprintGameplayTagLibrary::AppendGameplayTagContainers(FGameplayTagContainer& InOutTagContainer, const FGameplayTagContainer& InTagContainer)
{
    Reflection::FParams Params(StaticFunction_AppendGameplayTagContainers());
    Params.Set(L"InOutTagContainer", InOutTagContainer);
    Params.Set(L"InTagContainer", InTagContainer);
    if (Params.Call(StaticClass()->GetDefaultObject()))
    {
        InOutTagContainer = Params.Take<FGameplayTagContainer>(L"InOutTagContainer");
    }
}
