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

bool UBlueprintGameplayTagLibrary::DoesContainerMatchTagQuery(const FGameplayTagContainer& TagContainer, const FGameplayTagQuery& TagQuery)
{
    Reflection::FParams Params(StaticFunction_DoesContainerMatchTagQuery());
    Params.Set(L"TagContainer", TagContainer);
    Params.Set(L"TagQuery", TagQuery);
    return Params.Call(StaticClass()->GetDefaultObject()) && Params.Take<bool>(L"ReturnValue");
}
