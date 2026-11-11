#pragma once

enum class ESpawnActorCollisionHandlingMethod : uint8_t
{
    Undefined,
    AlwaysSpawn,
    AdjustIfPossibleButAlwaysSpawn,
    AdjustIfPossibleButDontSpawnIfColliding,
    DontSpawnIfColliding,
};

enum class ELevelCollectionType
{
    DynamicSourceLevels,
    DynamicDuplicatedLevels,
    StaticLevels,
};

// Unique handle that can be used to distinguish timers that have identical delegates (FTimerManager).
struct FTimerHandle
{
    bool IsValid() const
    {
        return Handle != 0;
    }

    void Invalidate()
    {
        Handle = 0;
    }

    bool operator==(const FTimerHandle& Other) const = default;

private:
    friend class FTimerManager;

    uint64_t Handle = 0;
};

static_assert(sizeof(FTimerHandle) == 8);
