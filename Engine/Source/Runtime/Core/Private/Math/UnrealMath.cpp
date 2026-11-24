#include "pch.h"
#include "Math/UnrealMathUtility.h"
#include "Math/Vector.h"
#include "Natives.h"

#include <cstdlib>

int32_t FMath::Rand()
{
    return Natives::CRT_rand();
}

float FMath::FRand()
{
    return Rand() / static_cast<float>(RAND_MAX);
}

float FMath::SRand()
{
    return Natives::FMath_SRand();
}

FVector FMath::VRandCone(const FVector& Dir, float ConeHalfAngleRad)
{
    const float Length = Dir.Size();
    if (Length <= 0.0f)
    {
        return Dir;
    }
    const FVector Forward = Dir * (1.0f / Length);
    if (ConeHalfAngleRad <= 0.0f)
    {
        return Forward;
    }
    // Epic's draw: Theta around the direction, Phi off it from the sphere's acos(2v - 1), folded into the cone by fmod
    // (UE4's FMath::VRandCone rotates Dir by Phi about its rotation's Y axis, then by Theta about Dir).
    const float Theta = 2.0f * 3.14159265f * FRand();
    const float Phi = std::fmod(std::acos(2.0f * FRand() - 1.0f), ConeHalfAngleRad);
    const FVector Up = std::fabs(Forward.Z) < 0.999f ? FVector(0.0f, 0.0f, 1.0f) : FVector(1.0f, 0.0f, 0.0f);
    FVector Right = FVector::CrossProduct(Up, Forward);
    Right = Right * (1.0f / Right.Size());
    const FVector Across = FVector::CrossProduct(Forward, Right);
    return Forward * std::cos(Phi) + (Right * std::cos(Theta) + Across * std::sin(Theta)) * std::sin(Phi);
}
