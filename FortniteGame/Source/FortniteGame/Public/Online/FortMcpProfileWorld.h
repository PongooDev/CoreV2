#pragma once

#include "UObject/Object.h"

class AFortPlayerController;

class UFortMcpProfileWorld : public UObject
{
    DefineUnrealClass(UFortMcpProfileWorld, FortniteGame)

public:
    void* BindToPlayerController(AFortPlayerController* PlayerController, bool bUseOutpostInventory);

    static void HookVTables();

private:
    static inline void* (*BindToPlayerControllerOG)(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory) = nullptr;
    static void* BindToPlayerControllerHK(UFortMcpProfileWorld* This, AFortPlayerController* PlayerController, bool bUseOutpostInventory);
};
