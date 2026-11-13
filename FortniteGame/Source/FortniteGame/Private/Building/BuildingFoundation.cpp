#include "pch.h"
#include "Building/BuildingFoundation.h"
#include "Finder.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "ReflectionParams.h"
#include "UObject/Class.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortBuilding, Log, All);

void ABuildingFoundation::SetDynamicFoundationTransform(const FTransform& NewTransform)
{
#if WITH_SERVER_CODE
    // The client builds keep the UFunction and strip its body (6.00's exec, CL-4395664 0x13CD9D0, steps the transform and
    // calls an empty stub). What is left around it shows what it did:
    // - The transform is replicated with an OnRep (OnRep_DynamicFoundationTransform, 0xD0D660), which moves the plot's
    //   root and the streamed level's actors to it on each client. The server runs no OnRep of its own, so it calls it
    //   here: the plot and its level stand where the clients see them.
    // - The plot's PostUpdate (0xD11960), run when the world manager updates the world's building actors, streams a
    //   Static plot's level in at once and a dynamic one's only once bTransformSet is set, which nothing in the build
    //   sets: this did. So a dynamic plot's level streams in where it is first placed.
    // - That needs a stream-in here too when PostUpdate has been and gone (bPostUpdateCalled): the floating island's
    //   level Blueprint calls this from the island's calendar events, after the world is up.
    // Without that, the floating island's plot (Athena_POI_Foundations' LF_Athena_StreamingTest13, Dynamic_StartEnabled,
    // the Lake Island: Athena_POI_LakeIsland_002) never streamed in on the server, while each client streams it in itself
    // as it comes near (the plot's start-up, 0xCF1670, registers it with the client's own streaming). The client then says
    // the island's /Temp/ level is visible (APlayerController::ServerUpdateLevelVisibility), the server has no such
    // package, and "ServerUpdateLevelVisibility() ignored non-existant package" closes its connection: near the island,
    // players were kicked (#401).
    if (Role != ROLE_Authority || Natives::ABuildingFoundation_bTransformSet < 0)
    {
        return;
    }

    FlushNetDormancy();
    DynamicFoundationTransform = NewTransform;
    bTransformSet = true;
    OnRep_DynamicFoundationTransform();

    const bool bStreamIn = bPostUpdateCalled && !bConditionalFoundation && !LevelStreamInfo;
    if (bStreamIn)
    {
        SelectAndSetupMyBuildingLevel(nullptr);
    }
    const FVector Location = NewTransform.GetLocation();
    UE_LOG(LogFortBuilding, Log, TEXT("SetDynamicFoundationTransform: %s is at (%.0f, %.0f, %.0f)%s"), *GetName(), Location.X, Location.Y, Location.Z,
        bStreamIn ? (LevelStreamInfo ? TEXT(", its level streaming in") : TEXT(", its level not streamed (no level picked)")) : TEXT(""));
#endif
}

void ABuildingFoundation::OnRep_DynamicFoundationTransform()
{
    Reflection::FParams Params(StaticFunction_OnRep_DynamicFoundationTransform());
    Params.Call(this);
}

bool ABuildingFoundation::SelectAndSetupMyBuildingLevel(FFortBuildingRandomList* RandomList)
{
    return Natives::ABuildingFoundation_SelectAndSetupMyBuildingLevel && Natives::ABuildingFoundation_SelectAndSetupMyBuildingLevel(this, RandomList);
}

DEFINE_FUNCTION(ABuildingFoundation::execSetDynamicFoundationTransform)
{
    // 6.00's exec takes the transform by reference (P_GET_STRUCT_REF); the native only reads it.
    P_GET_STRUCT(FTransform, Z_Param_NewTransform);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<ABuildingFoundation*>(Context)->SetDynamicFoundationTransform(Z_Param_NewTransform);
    P_NATIVE_END;
}

void ABuildingFoundation::SetDynamicFoundationEnabled(bool bEnabled)
{
#if WITH_SERVER_CODE
    // 6.20 (CL-4497486) adds it and the client builds strip its body: 6.21's exec (0x1458650, code shared with
    // USkeletalMeshComponent::execSetUpdateAnimationInEditor) steps the bool and calls the shared empty function. Loot Lake
    // hangs on it: Athena_POI_Foundations' level Blueprint turns its three Dynamic_StartDisabled plots on and off from
    // BP_IslandScripting (SetupPOI on onPOISetup, the first POI calendar event of a match; SwapPOI on onPOISwap, the
    // Butterfly's end): POI0 LF_Lake1 (Athena_POI_Lake_002) and LF_FloatingIsland (Athena_POI_LakeIsland_003), POI1 and
    // the swap LF_Lake2 (Athena_POI_Lake_003), the swap the first two off. Stripped, none ever streamed: Loot Lake was a dry
    // crater before and after the Butterfly (CoreV2 #430).
    // What is left around it shows what it did (🟡 the body is inferred from it, as SetDynamicFoundationTransform's is):
    // - bFoundationEnabled is replicated with an OnRep (OnRep_FoundationEnabled(bOldEnabled), 0xD645E0) that, on a
    //   dynamic plot that has begun play and whose state changed, shows the plot's level and LOD actors (0xD7D050) or
    //   streams them out (0xD65660, then the world manager). The server runs no OnRep of its own, so it calls it here.
    // - The plot's PostUpdate (6.20 0xD66F80) streams a Dynamic_StartDisabled plot's level only while bFoundationEnabled
    //   is set, and the island scripting turns it on after that (its calendar events come after the world is up), so the
    //   stream-in happens here too when PostUpdate has been and gone: as SetDynamicFoundationTransform does, without
    //   which a client near the plot streams a level the server doesn't have and is kicked (#401).
    if (Role != ROLE_Authority || !HasFoundationEnabled() || DynamicFoundationType == static_cast<uint8_t>(EDynamicFoundationType::Static))
    {
        return;
    }
    const bool bOldEnabled = bFoundationEnabled;
    if (bOldEnabled == bEnabled)
    {
        return;
    }
    FlushNetDormancy();
    bFoundationEnabled = bEnabled;
    OnRep_FoundationEnabled(bOldEnabled);

    const bool bStreamIn = bEnabled && Natives::ABuildingFoundation_bPostUpdateCalled >= 0 && bPostUpdateCalled && !bConditionalFoundation && !LevelStreamInfo;
    if (bStreamIn)
    {
        SelectAndSetupMyBuildingLevel(nullptr);
    }
    UE_LOG(LogFortBuilding, Log, TEXT("SetDynamicFoundationEnabled: %s is %s%s"), *GetName(), bEnabled ? TEXT("on") : TEXT("off"),
        bStreamIn ? (LevelStreamInfo ? TEXT(", its level streaming in") : TEXT(", its level not streamed (no level picked)")) : TEXT(""));
#endif
}

void ABuildingFoundation::OnRep_FoundationEnabled(bool bOldEnabled)
{
    Reflection::FParams Params(StaticFunction_OnRep_FoundationEnabled());
    Params.Set(L"bOldEnabled", bOldEnabled);
    Params.Call(this);
}

DEFINE_FUNCTION(ABuildingFoundation::execSetDynamicFoundationEnabled)
{
    P_GET_UBOOL(Z_Param_bEnabled);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<ABuildingFoundation*>(Context)->SetDynamicFoundationEnabled(Z_Param_bEnabled);
    P_NATIVE_END;
}

void ABuildingFoundation::HookNatives()
{
#if WITH_SERVER_CODE
    // Dynamic foundations are 6.00's (the floating island); older builds have no SetDynamicFoundationTransform. 6.20 adds
    // SetDynamicFoundationEnabled (Loot Lake's plots). Each is restored where the build's exec calls a stub.
    UClass* Class = StaticClass();
    UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
    if (!FindStaticFunction_SetDynamicFoundationTransform() || !Default || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }
    const auto IsStripped = [](UFunction* Function)
    {
        uint8_t* NativeCall = Function ? Finder::ImplementationCall(Function) : nullptr;
        return NativeCall && Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5));
    };
    UFunction* const Transform = FindStaticFunction_SetDynamicFoundationTransform();
    UFunction* const Enabled = FindStaticFunction_SetDynamicFoundationEnabled();
    const bool bTransformStripped = IsStripped(Transform);
    const bool bEnabledStripped = Enabled && IsStripped(Enabled);
    if (!bTransformStripped)
    {
        Log::Info(L"Finder: ABuildingFoundation::SetDynamicFoundationTransform is native in this build");
    }
    if (Enabled && !bEnabledStripped)
    {
        Log::Info(L"Finder: ABuildingFoundation::SetDynamicFoundationEnabled is native in this build");
    }
    if (!bTransformStripped && !bEnabledStripped)
    {
        return;
    }
    if (!Member_DynamicFoundationType.IsAvailable() || !Member_DynamicFoundationTransform.IsAvailable() || !Member_LevelStreamInfo.IsAvailable()
        || !Finder::ResolveBuildingFoundationLive(*reinterpret_cast<void***>(Default), Member_DynamicFoundationType.GetOffsetChecked()))
    {
        Log::Error(L"Hook ABuildingFoundation::SetDynamicFoundationTransform{}: not restored, so a dynamic foundation's level (the floating island's{}) never streams in on the server",
            bEnabledStripped ? L" and SetDynamicFoundationEnabled" : L"", bEnabledStripped ? L", Loot Lake's" : L"");
        return;
    }
    if (bTransformStripped)
    {
        Transform->SetNativeFunc(&execSetDynamicFoundationTransform);
        Log::Info(L"Hook ABuildingFoundation::SetDynamicFoundationTransform: native");
    }
    if (bEnabledStripped)
    {
        if (!HasFoundationEnabled() || !FindStaticFunction_OnRep_FoundationEnabled())
        {
            Log::Error(L"Hook ABuildingFoundation::SetDynamicFoundationEnabled: not restored (bFoundationEnabled or OnRep_FoundationEnabled not found), so Loot Lake's plots never stream in");
            return;
        }
        Enabled->SetNativeFunc(&execSetDynamicFoundationEnabled);
        Log::Info(L"Hook ABuildingFoundation::SetDynamicFoundationEnabled: native");
    }
#endif
}
