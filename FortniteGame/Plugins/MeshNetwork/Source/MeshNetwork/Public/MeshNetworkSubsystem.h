#pragma once

#include "Reflection.h"
#include "UObject/Object.h"
#include "UObject/ScriptDelegates.h"

#include <cstdint>

// EMeshNetworkNodeType (MeshNetwork plugin; 8.51's values, its UEnum's order).
enum class EMeshNetworkNodeType : uint8_t
{
    Root,
    Inner,
    Edge,
    Client,
    Unknown
};

// The game instance's mesh network state (MeshNetwork plugin, from 6.30). Epic's game servers formed a tree (a root, inner
// nodes, the match servers as edges) over mesh beacons; AFortGameSession::SetMeshNodeType (8.51 0x153F1B0, from the
// MeshNode= command line with -EnableMeshNetwork) sets the node type and joins the tree, and the clients get their
// server's type through AFortGameStateAthena::MeshNetworkStatus. UFortAsyncAction_MeshNetworkReady (8.51 0xF10120) waits
// for a node type other than Unknown (OnMeshNodeTypeChanged) before it answers.
class UMeshNetworkSubsystem : public UObject
{
    DefineUnrealClass(UMeshNetworkSubsystem, MeshNetwork)

public:
    DefineUProperty(FMulticastScriptDelegate, OnMeshNodeTypeChanged)
    DefineUProperty(FMulticastScriptDelegate, OnConnectedToRootChanged)
    DefineUProperty(uint8_t, NodeType)
    DefineUProperty(bool, bConnectedToRoot)

    static bool HasNodeType() { return StaticClass() && Member_NodeType.IsAvailable(); }

    // The world's game instance's (USubsystemBlueprintLibrary::GetGameInstanceSubsystem); null without one.
    static UMeshNetworkSubsystem* Get(const UObject* WorldContextObject);

    EMeshNetworkNodeType GetMeshNetworkNodeType() const { return static_cast<EMeshNetworkNodeType>(NodeType); }

    // Epic's (8.51 0x939700): sets the type and broadcasts OnMeshNodeTypeChanged.
    void SetMeshNetworkNodeType(EMeshNetworkNodeType InNodeType);
    // Epic's (8.51 0x939650): broadcasts OnConnectedToRootChanged when it changes.
    void SetConnectedToRoot(bool bInConnectedToRoot);
};
