#pragma once

#include "Math/Vector.h"
#include "UObject/Object.h"

class UKismetMathLibrary : public UObject
{
    DefineUnrealClass(UKismetMathLibrary, Engine)
    DefineUFunction(RandomUnitVector)
    DefineUFunction(RandomUnitVectorInCone)

public:
    static FVector RandomUnitVector();
    static FVector RandomUnitVectorInCone(const FVector& ConeDir, float ConeHalfAngle);
};
