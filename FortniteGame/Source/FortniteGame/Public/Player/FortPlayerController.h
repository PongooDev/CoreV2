#pragma once

#include "GameFramework/PlayerController.h"

class AFortQuickBars;

class AFortPlayerController : public APlayerController
{
    DefineUnrealClass(AFortPlayerController, FortniteGame)

public:
    DefineUProperty(AFortQuickBars*, QuickBars)

    bool InitializeMcpProfile(const void* OnComplete);
    void SpawnQuickBars();
    void SetupQuickBars();

    static void Hook();

private:
    static inline bool (*InitializeMcpProfileOG)(AFortPlayerController* This, const void* OnComplete) = nullptr;
    static bool InitializeMcpProfileHK(AFortPlayerController* This, const void* OnComplete);
};
