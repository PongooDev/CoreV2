#include "pch.h"
#include "PlaysetLevelStreamComponent.h"
#include "GameFramework/Actor.h"
#include "Natives.h"
#include "ReflectionParams.h"

void UPlaysetLevelStreamComponent::SetPlayset(UObject* NewPlayset)
{
    Reflection::FParams Params(FindStaticFunction_SetPlayset(), true);
    if (Params.Set(L"NewPlayset", NewPlayset))
    {
        Params.Call(this);
    }
}

FMulticastScriptDelegate* UPlaysetLevelStreamComponent::LoadPlaysetAsyncNoCallback()
{
    return Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback
        ? static_cast<FMulticastScriptDelegate*>(Natives::UPlaysetLevelStreamComponent_LoadPlaysetAsyncNoCallback(this)) : nullptr;
}

FMulticastScriptDelegate* UPlaysetLevelStreamComponent::UnloadPlaysetAsyncNoCallback()
{
    return Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback
        ? static_cast<FMulticastScriptDelegate*>(Natives::UPlaysetLevelStreamComponent_UnloadPlaysetAsyncNoCallback(this)) : nullptr;
}

UPlaysetLevelStreamComponent* UPlaysetLevelStreamComponent::GetForActor(AActor* Actor)
{
    UClass* ComponentClass = StaticClass();
    const TArray<UActorComponent*> Components = Actor && ComponentClass ? Actor->GetComponentsByClass(ComponentClass) : TArray<UActorComponent*>();
    return Components.Num() > 0 ? static_cast<UPlaysetLevelStreamComponent*>(Components[0]) : nullptr;
}
