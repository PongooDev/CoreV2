#pragma once

#include "Engine/GameInstance.h"

class UFortGameInstance : public UGameInstance
{
    DefineUnrealClass(UFortGameInstance, FortniteGame)

public:
    static void Hook();
};
