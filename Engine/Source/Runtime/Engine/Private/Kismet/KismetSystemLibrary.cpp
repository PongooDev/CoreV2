#include "pch.h"
#include "Kismet/KismetSystemLibrary.h"
#include "ReflectionParams.h"

#include <vector>

void UKismetSystemLibrary::GetComponentBounds(const USceneComponent* Component, FVector& Origin, FVector& BoxExtent, float& SphereRadius)
{
    Reflection::FParams Params(StaticFunction_GetComponentBounds());
    Params.Set(L"Component", Component);
    Origin = FVector();
    BoxExtent = FVector();
    SphereRadius = 0.0f;
    if (Component && Params.Call(StaticClass()->GetDefaultObject()))
    {
        Origin = Params.Take<FVector>(L"Origin");
        BoxExtent = Params.Take<FVector>(L"BoxExtent");
        SphereRadius = Params.Take<float>(L"SphereRadius");
    }
}

bool UKismetSystemLibrary::LineTraceSingle(UObject* WorldContextObject, const FVector& Start, const FVector& End, uint8_t TraceChannel, bool bTraceComplex, bool bIgnoreSelf, FVector& OutImpactPoint)
{
    // OutHit is an FHitResult, read through its own reflection (ImpactPoint is an FVector_NetQuantize, an FVector).
    UFunction* Function = StaticFunction_LineTraceSingle();
    const UStructProperty* OutHitProperty = Function ? static_cast<const UStructProperty*>(Function->FindPropertyByName(FName(L"OutHit", FNAME_Find))) : nullptr;
    UScriptStruct* HitStruct = OutHitProperty ? OutHitProperty->Struct : nullptr;
    const UProperty* ImpactPointProperty = HitStruct ? HitStruct->FindPropertyByName(FName(L"ImpactPoint", FNAME_Find)) : nullptr;
    if (!WorldContextObject || !ImpactPointProperty || ImpactPointProperty->ElementSize != sizeof(FVector))
    {
        return false;
    }

    Reflection::FParams Params(Function, true);
    Params.Set(L"WorldContextObject", WorldContextObject);
    Params.Set(L"Start", Start);
    Params.Set(L"End", End);
    Params.Set(L"TraceChannel", TraceChannel);
    Params.Set(L"bTraceComplex", bTraceComplex);
    Params.Set(L"bIgnoreSelf", bIgnoreSelf);
    std::vector<uint8_t> Hit(OutHitProperty->ElementSize, 0);
    if (!Params.Call(StaticClass()->GetDefaultObject()) || !Params.Take<bool>(L"ReturnValue") || !Params.Relocate(L"OutHit", Hit.data(), Hit.size()))
    {
        return false;
    }
    OutImpactPoint = *reinterpret_cast<const FVector*>(Hit.data() + ImpactPointProperty->GetOffset_ForInternal());
    return true;
}

bool UKismetSystemLibrary::K2_SetTimer(UObject* Object, const wchar_t* FunctionName, float Time, bool bLooping)
{
    UFunction* Function = FindStaticFunction_K2_SetTimer();
    Reflection::FParams Params(Function, true);
    if (!Function || !Object || !Params.Set(L"Object", Object) || !Params.Set(L"FunctionName", FString(FunctionName)) || !Params.Set(L"Time", Time) || !Params.Set(L"bLooping", bLooping))
    {
        return false;
    }
    return Params.Call(StaticClass()->GetDefaultObject());
}

void UKismetSystemLibrary::K2_ClearTimer(UObject* Object, const wchar_t* FunctionName)
{
    UFunction* Function = FindStaticFunction_K2_ClearTimer();
    Reflection::FParams Params(Function, true);
    if (Function && Object && Params.Set(L"Object", Object) && Params.Set(L"FunctionName", FString(FunctionName)))
    {
        Params.Call(StaticClass()->GetDefaultObject());
    }
}
