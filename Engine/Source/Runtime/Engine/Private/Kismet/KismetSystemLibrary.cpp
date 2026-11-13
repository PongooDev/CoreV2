#include "pch.h"
#include "Kismet/KismetSystemLibrary.h"
#include "ObjectLayout.h"
#include "ReflectionParams.h"
#include "UObject/AssetPtr.h"
#include "UObject/UnrealType.h"
#include "UObject/WeakObjectPtr.h"

#include <cstring>
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

bool UKismetSystemLibrary::DoesImplementInterface(UObject* TestObject, UClass* Interface)
{
    Reflection::FParams Params(StaticFunction_DoesImplementInterface());
    return TestObject && Interface && Params.Set(L"TestObject", TestObject) && Params.Set(L"Interface", Interface)
        && Params.Call(StaticClass()->GetDefaultObject()) && Params.Take<bool>(L"ReturnValue");
}

bool UKismetSystemLibrary::LineTraceSingle(UObject* WorldContextObject, const FVector& Start, const FVector& End, uint8_t TraceChannel, bool bTraceComplex, bool bIgnoreSelf, FVector& OutImpactPoint)
{
    UObject* Actor = nullptr;
    UObject* Component = nullptr;
    return LineTraceSingle(WorldContextObject, Start, End, TraceChannel, bTraceComplex, bIgnoreSelf, OutImpactPoint, Actor, Component);
}

bool UKismetSystemLibrary::LineTraceSingle(UObject* WorldContextObject, const FVector& Start, const FVector& End, uint8_t TraceChannel, bool bTraceComplex, bool bIgnoreSelf, FVector& OutImpactPoint, UObject*& OutActor, UObject*& OutComponent, FVector* OutImpactNormal)
{
    OutActor = nullptr;
    OutComponent = nullptr;
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
    if (OutImpactNormal)
    {
        // ImpactNormal is an FVector_NetQuantizeNormal, an FVector.
        const UProperty* ImpactNormalProperty = HitStruct->FindPropertyByName(FName(L"ImpactNormal", FNAME_Find));
        *OutImpactNormal = ImpactNormalProperty && ImpactNormalProperty->ElementSize == sizeof(FVector)
            ? *reinterpret_cast<const FVector*>(Hit.data() + ImpactNormalProperty->GetOffset_ForInternal()) : FVector(0.0f, 0.0f, 1.0f);
    }
    // Actor and Component are weak pointers (8 bytes), not always 8-aligned in the hit (Actor is at 0x64 in 4.16).
    const auto ReadWeak = [&Hit, HitStruct](const wchar_t* Name) -> UObject*
    {
        const UProperty* Property = HitStruct->FindPropertyByName(FName(Name, FNAME_Find));
        if (!Property || Property->ElementSize != sizeof(FWeakObjectPtr))
        {
            return nullptr;
        }
        FWeakObjectPtr Weak;
        std::memcpy(&Weak, Hit.data() + Property->GetOffset_ForInternal(), sizeof(Weak));
        return Weak.Get();
    };
    OutActor = ReadWeak(L"Actor");
    OutComponent = ReadWeak(L"Component");
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

bool UKismetSystemLibrary::LoadAsset(UObject* WorldContextObject, const FString& AssetPath, const FLatentActionInfo& LatentInfo)
{
    // KismetSystemLibrary.cpp's LoadAsset: a FLoadAssetActionBase (its constructor's RequestAsyncLoad) added to the world's
    // latent action manager under LatentInfo's CallbackTarget and UUID; each frame's ProcessLatentActions asks its handle
    // whether the load is done, then runs OnLoaded (the OnLoaded delegate, left unbound here) and resumes LatentInfo.
    UFunction* Function = FindStaticFunction_LoadAsset();
    if (!Function || !WorldContextObject || AssetPath.Len() == 0)
    {
        return false;
    }
    Reflection::FParams Params(Function, true);
    // TSoftObjectPtr<UObject> (FSoftObjectPtr, 0x28 bytes from 1.10; TAssetPtr's 0x20 before): the path goes into its
    // FSoftObjectPath in the build's layout.
    UProperty* AssetProperty = Function->FindPropertyByName(FName(L"Asset", FNAME_Find));
    const size_t AssetSize = sizeof(TAssetPtr<UObject>) + (ObjectLayout::Current.bSoftObjectPaths ? Reflection::SoftPathGrowth<TAssetPtr<UObject>> : 0);
    TAssetPtr<UObject>* Asset = AssetProperty && static_cast<size_t>(AssetProperty->ElementSize) == AssetSize ? static_cast<TAssetPtr<UObject>*>(Params.GetAddress(L"Asset")) : nullptr;
    FLatentActionInfo* Info = Params.Get<FLatentActionInfo>(L"LatentInfo");
    if (!Asset || !Info || !Params.Set(L"WorldContextObject", WorldContextObject))
    {
        return false;
    }
    Asset->SetPath(AssetPath);
    *Info = LatentInfo;
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
