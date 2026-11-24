#include "pch.h"
#include "Athena/AthenaFillFloor.h"
#include "Finder.h"
#include "Natives.h"

bool AAthenaFillFloor::HasSetCapHeight()
{
#if WITH_SERVER_CODE
    // CapHeight follows FloorZ (8.40 FloorZ +0x704, CapHeight +0x708; 21.40 +0xA6C, +0xA70).
    return Natives::ABuildingFillFloor_SetCapHeight
        || (StaticClass() && Member_FloorZ.IsAvailable() && Finder::ResolveFillFloorSetCapHeightLive(Member_FloorZ.GetOffsetChecked() + static_cast<int32_t>(sizeof(float))));
#else
    return false;
#endif
}

bool AAthenaFillFloor::SetCapHeight(float NewCapHeight, bool bUpdateMovement)
{
#if WITH_SERVER_CODE
    if (!HasSetCapHeight())
    {
        return false;
    }
    Natives::ABuildingFillFloor_SetCapHeight(this, NewCapHeight, bUpdateMovement);
    return true;
#else
    return false;
#endif
}
