#include "pch.h"
#include "Math/UnrealMathUtility.h"
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
