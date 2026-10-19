#pragma once

#include "GameFramework/Info.h"
#include "Templates/SubclassOf.h"

class AGameSession;

class AGameModeBase : public AInfo
{
    DefineUnrealClass(AGameModeBase, Engine)

public:
    DefineUProperty(TSubclassOf<AGameSession>, GameSessionClass)
    DefineUProperty(AGameSession*, GameSession)
};
