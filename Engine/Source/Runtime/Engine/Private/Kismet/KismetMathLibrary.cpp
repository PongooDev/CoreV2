#include "pch.h"
#include "Kismet/KismetMathLibrary.h"
#include "ReflectionParams.h"

FVector UKismetMathLibrary::RandomUnitVector()
{
    Reflection::FParams Params(StaticFunction_RandomUnitVector());
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<FVector>(L"ReturnValue") : FVector();
}

FDateTime UKismetMathLibrary::UtcNow()
{
    Reflection::FParams Params(StaticFunction_UtcNow());
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<FDateTime>(L"ReturnValue") : FDateTime();
}

FVector UKismetMathLibrary::RandomUnitVectorInCone(const FVector& ConeDir, float ConeHalfAngle)
{
    UFunction* const InCone = FindStaticFunction_RandomUnitVectorInCone();
    Reflection::FParams Params(InCone ? InCone : StaticFunction_RandomUnitVectorInConeInRadians());
    Params.Set(L"ConeDir", ConeDir);
    Params.Set(InCone ? L"ConeHalfAngle" : L"ConeHalfAngleInRadians", ConeHalfAngle);
    return Params.Call(StaticClass()->GetDefaultObject()) ? Params.Take<FVector>(L"ReturnValue") : FVector();
}
