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

void ABuildingFoundation::HookNatives()
{
#if WITH_SERVER_CODE
    // Dynamic foundations are 6.00's (the floating island); older builds have no SetDynamicFoundationTransform.
    UFunction* Function = FindStaticFunction_SetDynamicFoundationTransform();
    UClass* Class = StaticClass();
    UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
    if (!Function || !Default || (!Natives::FFrame_Step && !Finder::ResolveScriptFrame()))
    {
        return;
    }
    uint8_t* NativeCall = Finder::ImplementationCall(Function);
    if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
    {
        Log::Info(L"Finder: ABuildingFoundation::SetDynamicFoundationTransform is native in this build");
        return;
    }
    if (!Member_DynamicFoundationType.IsAvailable() || !Member_DynamicFoundationTransform.IsAvailable() || !Member_LevelStreamInfo.IsAvailable()
        || !Finder::ResolveBuildingFoundationLive(*reinterpret_cast<void***>(Default), Member_DynamicFoundationType.GetOffsetChecked()))
    {
        Log::Error(L"Hook ABuildingFoundation::SetDynamicFoundationTransform: not restored, so a dynamic foundation's level (the floating island's) never streams in on the server");
        return;
    }
    Function->SetNativeFunc(&execSetDynamicFoundationTransform);
    Log::Info(L"Hook ABuildingFoundation::SetDynamicFoundationTransform: native");
#endif
}
