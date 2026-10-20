#pragma once

#include "GameFramework/PlayerController.h"

class AFortInventory;
class AFortQuickBars;
class UFortWorldItem;

class AFortPlayerController : public APlayerController
{
    DefineUnrealClass(AFortPlayerController, FortniteGame)

public:
    DefineUProperty(AFortQuickBars*, QuickBars)
    DefineUProperty(AFortInventory*, WorldInventory)

    bool InitializeMcpProfile(const void* OnComplete);
    void SpawnQuickBars();
    void SetupQuickBars();
    void AddDefaultInventory();
    void TryAddToQuickBar(UFortWorldItem* Item);

    static void Hook();

private:
    static inline bool (*InitializeMcpProfileOG)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    static bool InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete);
};
