#pragma once

#include "Engine/GameInstance.h"

class UFortGameInstance : public UGameInstance
{
    DefineUnrealClass(UFortGameInstance, FortniteGame)

public:
    DefineUProperty(UObject*, TooltipManager)

    void Init();

    static void Hook();

private:
    static inline void (*InitOG)(UFortGameInstance* This) = nullptr;
    static void InitHK(UFortGameInstance* This);
};
