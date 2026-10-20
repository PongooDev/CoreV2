#pragma once

#include "GameFramework/Actor.h"

class AFortAIDirector : public AActor
{
    DefineUnrealClass(AFortAIDirector, FortniteGame)

public:
    static AFortAIDirector* GetCurrent(const UObject* WorldContextObject);
};
