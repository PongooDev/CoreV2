#pragma once

#include "Building/BuildingSMActor.h"
#include "Math/Box.h"
#include "Math/Transform.h"
#include "ObjectLayout.h"
#include "Reflection.h"
#include "UObject/Stack.h"

class ULevelStreaming;

// 6.00's EDynamicFoundationType: a Static plot stays where it was placed; a Dynamic_StartEnabled one (the floating
// island's, Athena_POI_Foundations' LF_Athena_StreamingTest13) is moved by its DynamicFoundationTransform. 6.20 adds
// Dynamic_StartDisabled: the plot streams its level only once SetDynamicFoundationEnabled(true) (6.20's and 6.21's Loot
// Lake: LF_Lake1, LF_Lake2 and LF_FloatingIsland).
enum class EDynamicFoundationType : uint8_t
{
    Static = 0,
    Dynamic_StartEnabled = 1,
    Dynamic_StartDisabled = 2,
};

// A building's or a POI's plot: its static mesh is the plot, and the level it streams in (a building or POI sublevel,
// /Temp/Game/... once loaded) sits on it.
class ABuildingFoundation : public ABuildingSMActor
{
    DefineUnrealClass(ABuildingFoundation, FortniteGame)
    DefineUFunction(OnRep_DynamicFoundationTransform)
    DefineUFunction(SetDynamicFoundationTransform)
    DefineUFunction(SetDynamicFoundationEnabled)
    DefineUFunction(OnRep_FoundationEnabled)

public:
    // The streamed level's package (/Temp/Game/Athena/Maps/POI/Athena_POI_City_001_43); None until it streams.
    DefineUProperty(FName, LevelToStream)
    // The plot's box in the foundation's space. Present on every build; it is the only bounds Core reads. (The
    // editor-built FoundationBasedBoundingBox it sat beside until 3.0 became the transient
    // StreamingData.BoundingBox (FBuildingFoundationStreamingData) in 3.1, so Core declares neither.)
    DefineUProperty(FBox, StreamingBoundingBox)
    // The world manager's conditional foundation quota picks which of these stream; the plot's own PostUpdate skips them.
    DefineBitfieldUProperty(bConditionalFoundation)
    // From 6.00 (EDynamicFoundationType).
    DefineUProperty(uint8_t, DynamicFoundationType)
    // From 6.00: where a dynamic plot is (replicated; its OnRep moves the plot and its level on each client).
    DefineUProperty(FTransform, DynamicFoundationTransform)
    // From 6.20: whether a dynamic plot is on (replicated; OnRep_FoundationEnabled streams its level in or out on each
    // client). A Dynamic_StartDisabled plot is off until SetDynamicFoundationEnabled(true).
    DefineBitfieldUProperty(bFoundationEnabled)
    static bool HasFoundationEnabled() { return Member_bFoundationEnabled.IsAvailable(); }
    // The level streaming object for the plot's level, once StreamInMyBuilding has made it.
    DefineUProperty(ULevelStreaming*, LevelStreamInfo)
    // Unreflected (21.40's names; Finder::ResolveBuildingFoundationLive): the transform has been set, and the plot's
    // PostUpdate has run.
    DefineNativeBitfield(bTransformSet, ABuildingFoundation_bTransformSet, Natives::ABuildingFoundation_bTransformSetMask)
    DefineNativeBitfield(bPostUpdateCalled, ABuildingFoundation_bPostUpdateCalled, Natives::ABuildingFoundation_bPostUpdateCalledMask)

    // From 6.00: moves a dynamic plot, and on the server streams its level in once it has a place. The floating island's
    // level Blueprint (Athena_POI_Foundations' SetLevelPosition, bound to BP_IslandScripting's IslandPositionUpdate)
    // calls it with the island's position each time the calendar moves the island. The client builds strip its body.
    void SetDynamicFoundationTransform(const FTransform& NewTransform);
    // The build's own OnRep (6.00 0xD0D660): sets the plot's root to DynamicFoundationTransform and moves the streamed
    // level's actors with it.
    void OnRep_DynamicFoundationTransform();
    // From 6.20: turns a dynamic plot on or off. 6.20's and 6.21's Athena_POI_Foundations level Blueprint calls it from
    // BP_IslandScripting's onPOISetup and onPOISwap: Loot Lake's plots by the calendar's POI0 and POI1. The client
    // builds strip its body (BuildingFoundation.cpp).
    void SetDynamicFoundationEnabled(bool bEnabled);
    // The build's own OnRep (6.21 0xD645E0): with the plot begun play and the state changed, streams its level in (shows
    // it and its LOD actors) or out.
    void OnRep_FoundationEnabled(bool bOldEnabled);
    // Picks the plot's level (the world manager's saved one, else its own pick) and streams it in (6.00 0xD142E0).
    bool SelectAndSetupMyBuildingLevel(FFortBuildingRandomList* RandomList);

    DECLARE_FUNCTION(execSetDynamicFoundationTransform);
    DECLARE_FUNCTION(execSetDynamicFoundationEnabled);

    // From 6.00: SetDynamicFoundationTransform's native, and from 6.20 SetDynamicFoundationEnabled's, where the build's
    // is the empty stub. Nothing on older builds.
    static void HookNatives();
};
