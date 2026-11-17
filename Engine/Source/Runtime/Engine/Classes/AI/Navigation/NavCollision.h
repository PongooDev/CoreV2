#pragma once

#include "Reflection.h"
#include "UObject/Object.h"

class UBodySetup;

// A static mesh's navigation collision (UE 4.20's NavigationSystem module, which Fortnite takes from 4.2; UNavCollisionBase
// stays in Engine). UStaticMesh::PostLoad gives every mesh with navigation data one, through
// UNavCollisionBase::ConstructNewInstanceDelegate, and UStaticMeshComponent::IsNavigationRelevant needs it: a mesh without
// one adds nothing to the navigation octree, so no navmesh is built on it.
class UNavCollisionBase : public UObject
{
    DefineUnrealClass(UNavCollisionBase, Engine)

public:
    // UNavCollisionBase::ConstructNew: a new navigation collision for Outer, made by whatever ConstructNewInstanceDelegate
    // is bound to. Core calls it only once UNavCollision's class default has bound its own ConstructNew
    // (UNavCollision::CreateOnServer), so it is that one: NewObject<UNavCollision>(&Outer).
    static UNavCollisionBase* ConstructNew(UObject& Outer);

    // The build's own virtual (7.00 slot 0x47, 6.31 the same): the collision taken from the mesh's body setup, which in a
    // cooked game is the cooked navcollision data or nothing.
    void Setup(UBodySetup* BodySetup);
};

class UNavCollision : public UNavCollisionBase
{
    DefineUnrealClass(UNavCollision, NavigationSystem)
    // Whether the class default binds ConstructNewInstanceDelegate to UNavCollision::ConstructNew when the game isn't a
    // server (config; Fortnite's is off). From 4.2.
    DefineBitfieldUProperty(bCreateOnClient)

public:
    // UNavCollision::ConstructNew: NewObject<UNavCollision>(&Outer).
    static UNavCollisionBase* ConstructNew(UObject& Outer);

    // The build's own virtual. On the class default it binds UNavCollisionBase::ConstructNewInstanceDelegate to
    // UNavCollision::ConstructNew when GIsServer or bCreateOnClient is set (7.00 0x2D86B10, 6.31 0x2D43FB0).
    void PostInitProperties();

    // Save the World servers (#469): the navigation collision a server's static meshes have. UNavCollision's class
    // default ran PostInitProperties while PreInit loaded the modules, and on these packed client builds Core only makes
    // the game a server (GIsServer, 7.00 +0x5749085) once PreInit is over (FEngineLoop::HookAfterPreInit and
    // ServerAtStart). bCreateOnClient is off, so the delegate kept its default, which makes nothing, and every
    // UStaticMesh::PostLoad left its mesh without one: a headless 7.00 Zone_Onboarding_Suburban_a had 1304 meshes, all
    // with navigation data and a body setup, none with a navigation collision, and none of its 3741 primitive components
    // navigation relevant, so the husks' navmesh had nothing to be built on (no husk's or player's spot projected).
    // Core turns bCreateOnClient on and runs the class default's PostInitProperties again, so the engine binds
    // UNavCollision::ConstructNew itself, then gives the meshes already loaded theirs as PostLoad would have
    // (UStaticMesh::CreateNavCollision). Meshes loaded from then on get theirs in their own PostLoad. Nothing on builds
    // without bCreateOnClient (before 4.2).
    static void CreateOnServer();
};
