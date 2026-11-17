#pragma once

#include "Reflection.h"
#include "UObject/Object.h"

class UBodySetup;
class UNavCollisionBase;

class UStaticMesh : public UObject
{
    DefineUnrealClass(UStaticMesh, Engine)

public:
    DefineUProperty(UBodySetup*, BodySetup)
    DefineBitfieldUProperty(bHasNavigationData)
    DefineUProperty(UNavCollisionBase*, NavCollision)

    // UStaticMesh::CreateNavCollision, the end of UStaticMesh::PostLoad (inlined there: 7.00 0x2A7440D, 6.31 0x2A0C79D):
    // a mesh with navigation data and a body setup gets a navigation collision (UNavCollisionBase::ConstructNew) when it
    // has none, set up from its body setup.
    void CreateNavCollision(bool bIsUpdate = false);

    // The vtable slot of UNavCollisionBase::Setup, read off UStaticMesh::PostLoad's call of it (7.00 and 6.31 0x47,
    // `jmp qword ptr [rax+238h]` right after the new collision is stored); -1 if PostLoad doesn't have that shape.
    static int32_t NavCollisionSetupIndex();
};
