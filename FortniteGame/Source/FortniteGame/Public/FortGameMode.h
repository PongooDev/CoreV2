#pragma once

#include "GameFramework/GameMode.h"

class AFortGameMode : public AGameMode
{
    DefineUnrealClass(AFortGameMode, FortniteGame)

public:
    TSubclassOf<AGameSession> GetGameSessionClass() const;

    static void Hook();

private:
    static TSubclassOf<AGameSession>* GetGameSessionClassHK(const AFortGameMode* This, TSubclassOf<AGameSession>* Result);
};
