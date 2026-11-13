#pragma once

#include "Math/Vector.h"
#include "UObject/Object.h"

class USceneComponent;

// Epic's FLatentActionInfo (Engine/LatentActionManager.h): where a latent action resumes once it is done. The world's
// latent action manager calls CallbackTarget's ExecutionFunction with Linkage as its one int32 parameter (as a Blueprint's
// ExecuteUbergraph is called with its entry point); a Linkage of -1 (INDEX_NONE) resumes nothing.
struct FLatentActionInfo
{
    int32_t Linkage = -1;
    int32_t UUID = 0;
    FName ExecutionFunction;
    UObject* CallbackTarget = nullptr;
};
static_assert(sizeof(FLatentActionInfo) == 0x18);

class UKismetSystemLibrary : public UObject
{
    DefineUnrealClass(UKismetSystemLibrary, Engine)
    DefineUFunction(GetComponentBounds)
    DefineUFunction(LineTraceSingle)
    DefineUFunction(K2_SetTimer)
    DefineUFunction(K2_ClearTimer)
    DefineUFunction(DoesImplementInterface)
    DefineUFunction(LoadAsset)

public:
    // The Blueprint "Async Load Asset" (UKismetSystemLibrary::LoadAsset, 3.0's to 6.21's): AssetPath ("/Game/Package.Asset")
    // loads on the asset manager's FStreamableManager (RequestAsyncLoad, off the game thread, its objects' post-load spread
    // over frames), and once it has (or failed to), the world's latent action manager resumes LatentInfo. False where the
    // build has no such function, or it couldn't be called.
    static bool HasLoadAsset() { return FindStaticFunction_LoadAsset() != nullptr; }
    static bool LoadAsset(UObject* WorldContextObject, const FString& AssetPath, const FLatentActionInfo& LatentInfo);
    // TestObject's class implements Interface, natively or in a blueprint (UClass::ImplementsInterface).
    static bool DoesImplementInterface(UObject* TestObject, UClass* Interface);
    static void GetComponentBounds(const USceneComponent* Component, FVector& Origin, FVector& BoxExtent, float& SphereRadius);
    // The first blocking hit's impact point from Start to End on a trace channel (ETraceTypeQuery: 0 is Visibility), no
    // actors ignored but the world context object when bIgnoreSelf. False when nothing blocks.
    static bool LineTraceSingle(UObject* WorldContextObject, const FVector& Start, const FVector& End, uint8_t TraceChannel, bool bTraceComplex, bool bIgnoreSelf, FVector& OutImpactPoint);
    // The same, with the hit's actor and component (FHitResult's weak Actor and Component; null when the hit has none),
    // and its ImpactNormal when OutImpactNormal is given.
    static bool LineTraceSingle(UObject* WorldContextObject, const FVector& Start, const FVector& End, uint8_t TraceChannel, bool bTraceComplex, bool bIgnoreSelf, FVector& OutImpactPoint, UObject*& OutActor, UObject*& OutComponent, FVector* OutImpactNormal = nullptr);
    // Runs Object's UFunction FunctionName (no parameters) every Time seconds when bLooping, else once after Time, on
    // Object's world's timer manager. K2_ClearTimer clears the timers set for it so.
    static bool K2_SetTimer(UObject* Object, const wchar_t* FunctionName, float Time, bool bLooping);
    static void K2_ClearTimer(UObject* Object, const wchar_t* FunctionName);
};
