#pragma once

#include <cstdint>

// A date and time: 100-nanosecond ticks since 0001-01-01 (Misc/DateTime.h). Core reads the current one with
// UKismetMathLibrary::UtcNow.
struct FDateTime
{
    int64_t Ticks = 0;

    int64_t GetTicks() const { return Ticks; }
    // Seconds from Other to this one (FTimespan's GetTotalSeconds of the difference).
    double SecondsSince(const FDateTime& Other) const { return static_cast<double>(Ticks - Other.Ticks) / 10000000.0; }
};

static_assert(sizeof(FDateTime) == 0x8);
