#include "pch.h"
#include "Online/FortMcpProfileWorld.h"
#include "Finder.h"
#include "Hooks.h"
#include "Natives.h"
#include "UObject/Class.h"
#include "UObject/UObjectIterator.h"

void* UFortMcpProfileWorld::BindToPlayerController(AFortPlayerController* PlayerController, bool bUseOutpostInventory)
{
    void* const Result = BindToPlayerControllerOG(this, PlayerController, bUseOutpostInventory);

#if WITH_SERVER_CODE
    uint8_t* const Profile = reinterpret_cast<uint8_t*>(this);
    if (*reinterpret_cast<void**>(Profile + Natives::UFortMcpProfileWorld_Inventory))
    {
        Profile[Natives::UFortMcpProfileWorld_bIsInventoryInitialized] = 1;
    }
#endif
    return Result;
}

void UFortMcpProfileWorld::HookVTables()
{
    UObject* ProfileDefault = StaticClass()->GetDefaultObject(false);
    void** ProfileVTable = ProfileDefault ? *reinterpret_cast<void***>(ProfileDefault) : nullptr;
    if (!ProfileVTable || !Finder::ResolveMcpProfileWorldLive(ProfileVTable))
    {
        return;
    }

    const int32_t Index = Natives::UFortMcpProfileWorld_BindToPlayerControllerIndex;
    void* const Bind = ProfileVTable[Index];
    BindToPlayerControllerOG = reinterpret_cast<decltype(BindToPlayerControllerOG)>(Bind);
    for (TObjectIterator<UClass> It; It; ++It)
    {
        UClass* Class = *It;
        UObject* ClassDefault = Class->IsChildOf(StaticClass()) ? Class->GetDefaultObject(false) : nullptr;
        void** VTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (VTable && VTable[Index] == Bind)
        {
            Hooks::SwapVTable(L"UFortMcpProfileWorld::BindToPlayerController", VTable, Index, reinterpret_cast<void*>(&BindToPlayerControllerHK));
        }
    }
}

void* UFortMcpProfileWorld::BindToPlayerControllerHK(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory)
{
    return This->BindToPlayerController(PlayerController, bUseOutpostInventory);
}
