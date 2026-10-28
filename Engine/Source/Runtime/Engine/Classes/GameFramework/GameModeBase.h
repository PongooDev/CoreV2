#pragma once

#include "GameFramework/Info.h"
#include "Templates/SubclassOf.h"

class AController;
class AGameSession;
class UClass;

class AGameModeBase : public AInfo
{
    DefineUnrealClass(AGameModeBase, Engine)
    DefineUFunction(GetDefaultPawnClassForController)
    DefineUFunction(RestartPlayer)

public:
    DefineUProperty(TSubclassOf<AGameSession>, GameSessionClass)
    DefineUProperty(AGameSession*, GameSession)

    UClass* GetDefaultPawnClassForController(AController* InController);
    void RestartPlayer(AController* NewPlayer);
};
