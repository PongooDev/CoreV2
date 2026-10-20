#pragma once

#include "GameFramework/PlayerController.h"
#include "Misc/Guid.h"

class AFortInventory;
class AFortPawn;
class AFortQuickBars;
class UFortWorldItem;

class AFortPlayerController : public APlayerController
{
    DefineUnrealClass(AFortPlayerController, FortniteGame)

public:
    DefineUProperty(AFortQuickBars*, QuickBars)
    DefineUProperty(AFortInventory*, WorldInventory)
    DefineUProperty(AFortPawn*, MyFortPawn)
    DefineUFunction(ServerExecuteInventoryItem)

    bool InitializeMcpProfile(const void* OnComplete);
    void SpawnQuickBars();
    void SetupQuickBars();
    void AddDefaultInventory();
    void TryAddToQuickBar(UFortWorldItem* Item);
    void ServerExecuteInventoryItem_Implementation(const FGuid& ItemGuid);

    static void Hook();
    static void HookVTables();

private:
    static inline bool (*InitializeMcpProfileOG)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    static bool InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete);
    static void ServerExecuteInventoryItem_ImplementationHK(AFortPlayerController* This, const FGuid& ItemGuid);
};
