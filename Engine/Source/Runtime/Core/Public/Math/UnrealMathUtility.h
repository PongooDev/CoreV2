#pragma once

#include <cmath>
#include <cstdint>

struct FMath
{
    template <class T>
    static constexpr T Max(const T A, const T B)
    {
        return (A >= B) ? A : B;
    }

    template <class T>
    static constexpr T Min(const T A, const T B)
    {
        return (A <= B) ? A : B;
    }

    template <class T>
    static constexpr T Clamp(const T X, const T MinValue, const T MaxValue)
    {
        return X < MinValue ? MinValue : X < MaxValue ? X : MaxValue;
    }

    template <class T>
    static constexpr T Abs(const T A)
    {
        return (A >= static_cast<T>(0)) ? A : -A;
    }

    template <class T>
    static constexpr T Square(const T A)
    {
        return A * A;
    }

    template <class T, class U>
    static T Lerp(const T& A, const T& B, const U& Alpha)
    {
        return static_cast<T>(A + Alpha * (B - A));
    }

    static int32_t TruncToInt(float F)
    {
        return static_cast<int32_t>(F);
    }

    static int32_t FloorToInt(float F)
    {
        return TruncToInt(std::floor(F));
    }

    static int32_t RoundToInt(float F)
    {
        return FloorToInt(F + 0.5f);
    }

    static float Sqrt(float Value)
    {
        return std::sqrt(Value);
    }

    static float InvSqrt(float F)
    {
        return 1.0f / std::sqrt(F);
    }

    static int32_t Rand();
    static float FRand();
    static float SRand();
};
