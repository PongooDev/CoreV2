#pragma once

#include "GameFramework/Info.h"
#include "Templates/SubclassOf.h"

class AController;
class AGameSession;
class APlayerController;
class UClass;

class AGameModeBase : public AInfo
{
    DefineUnrealClass(AGameModeBase, Engine)
    DefineUFunction(GetDefaultPawnClassForController)
    DefineUFunction(RestartPlayer)

public:
    DefineUProperty(TSubclassOf<AGameSession>, GameSessionClass)
    DefineUProperty(AGameSession*, GameSession)
    // The class of the players' controllers (Athena_PlayerController_C in Battle Royale).
    DefineUProperty(TSubclassOf<APlayerController>, PlayerControllerClass)

    UClass* GetDefaultPawnClassForController(AController* InController);
    void RestartPlayer(AController* NewPlayer);
};
