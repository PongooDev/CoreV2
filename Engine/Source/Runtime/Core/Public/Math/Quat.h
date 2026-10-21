#pragma once

struct FRotator;

struct alignas(16) FQuat
{
    float X = 0.0f;
    float Y = 0.0f;
    float Z = 0.0f;
    float W = 1.0f;

    FRotator Rotator() const;
};

static_assert(sizeof(FQuat) == 0x10);
