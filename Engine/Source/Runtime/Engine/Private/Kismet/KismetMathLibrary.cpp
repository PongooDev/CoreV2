#include "pch.h"
#include "Kismet/KismetMathLibrary.h"
#include "ReflectionParams.h"

FVector UKismetMathLibrary::RandomUnitVector()
{
    Reflection::FParams Params(StaticFunction_RandomUnitVector());
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<FVector>(L"ReturnValue") : FVector();
}

FVector UKismetMathLibrary::RandomUnitVectorInCone(const FVector& ConeDir, float ConeHalfAngle)
{
    Reflection::FParams Params(StaticFunction_RandomUnitVectorInCone());
    Params.Set(L"ConeDir", ConeDir);
    Params.Set(L"ConeHalfAngle", ConeHalfAngle);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<FVector>(L"ReturnValue") : FVector();
}
