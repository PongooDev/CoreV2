#pragma once

#include "Containers/Array.h"
#include "Engine/GameInstance.h"

class UFortRegisteredPlayerInfo;

class UFortGameInstance : public UGameInstance
{
    DefineUnrealClass(UFortGameInstance, FortniteGame)

public:
    DefineUProperty(UObject*, TooltipManager)
    DefineUProperty(TArray<UFortRegisteredPlayerInfo*>, RegisteredPlayers)

    void Init();

    static void Hook();

private:
    static inline void (*InitOG)(UFortGameInstance* This) = nullptr;
    static void InitHK(UFortGameInstance* This);
};
