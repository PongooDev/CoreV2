#include "pch.h"
#include "Athena/AthenaBigBaseWall.h"
#include "Engine/World.h"
#include "Log.h"
#include "Math/Rotator.h"
#include "Math/Transform.h"
#include "Math/Vector.h"
#include "Templates/Casts.h"
#include "UObject/Class.h"

#include <cmath>

namespace
{
    // An unbound native multicast delegate as the class's constructor leaves it: TArray<FDelegateBase> InvocationList
    // (no data, nothing in it), then CompactionThreshold and InvocationListLockCount (0x18 bytes in all).
    bool IsUnboundMulticastDelegate(const uint8_t* Delegate)
    {
        const int32_t ArrayNum = *reinterpret_cast<const int32_t*>(Delegate + 8);
        const int32_t ArrayMax = *reinterpret_cast<const int32_t*>(Delegate + 12);
        const int32_t LockCount = *reinterpret_cast<const int32_t*>(Delegate + 20);
        return ArrayNum == 0 && ArrayMax >= 0 && ArrayMax < 64 && LockCount == 0;
    }
}

TMulticastDelegate<>* AAthenaBigBaseWall::GetNativeDelegate(int32_t OffsetFromEnd) const
{
    // 6.30's AAthenaBigBaseWall is 0x7C0 bytes: its last reflected member, ZKillLevel, ends at +0x770, then come its
    // unreflected members, the two delegates last (+0x790 and +0x7A8, 0x18 bytes each). They are read from the end of
    // the class, and only taken while they look as the constructor leaves them (or as Core bound them).
    UClass* Class = StaticClass();
    const int32_t Size = Class ? Class->GetPropertiesSize() : 0;
    if (Size < OffsetFromEnd + 0x18)
    {
        return nullptr;
    }
    const uint8_t* Delegate = reinterpret_cast<const uint8_t*>(this) + Size - OffsetFromEnd;
    const int32_t ArrayNum = *reinterpret_cast<const int32_t*>(Delegate + 8);
    if (!IsUnboundMulticastDelegate(Delegate) && (ArrayNum < 0 || ArrayNum > 4))
    {
        return nullptr;
    }
    return reinterpret_cast<TMulticastDelegate<>*>(const_cast<uint8_t*>(Delegate));
}

TMulticastDelegate<>* AAthenaBigBaseWall::GetOnWallStartingToComeDown() const
{
    return GetNativeDelegate(0x30);
}

TMulticastDelegate<>* AAthenaBigBaseWall::GetOnWallDown() const
{
    return GetNativeDelegate(0x18);
}

AAthenaBigBaseWall* AAthenaBigBaseWall::CreateWall(UWorld* World, UClass* BigBaseWallClass, const FVector& MidlineStart, const FVector& MidlineEnd, float ZLevel)
{
#if WITH_SERVER_CODE
    if (!World || !BigBaseWallClass)
    {
        return nullptr;
    }
    // The wall stands at the middle of the line, at ZLevel, turned along it.
    const FVector Direction = MidlineEnd - MidlineStart;
    constexpr float RadiansToDegrees = 180.0f / 3.1415926535897932f;
    const float Yaw = std::atan2(Direction.Y, Direction.X) * RadiansToDegrees;
    FVector Location = MidlineStart + Direction * 0.5f;
    Location.Z = ZLevel;

    FActorSpawnParameters SpawnParameters;
    SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    FTransform SpawnTransform;
    SpawnTransform.Rotation = FRotator(0.0f, Yaw, 0.0f).Quaternion();
    SpawnTransform.SetLocation(Location);
    return Cast<AAthenaBigBaseWall>(World->SpawnActor(BigBaseWallClass, &SpawnTransform, SpawnParameters));
#else
    return nullptr;
#endif
}
