#pragma once

#include "Math/Vector.h"
#include "UObject/Object.h"

class UKismetMathLibrary : public UObject
{
    DefineUnrealClass(UKismetMathLibrary, Engine)
    DefineUFunction(RandomUnitVector)
    DefineUFunction(RandomUnitVectorInCone)
    // 1.10's (UE 4.19) name for it, with ConeHalfAngleInRadians.
    DefineUFunction(RandomUnitVectorInConeInRadians)

public:
    static FVector RandomUnitVector();
    static FVector RandomUnitVectorInCone(const FVector& ConeDir, float ConeHalfAngle);
};
