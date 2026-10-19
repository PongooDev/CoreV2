#pragma once

#include "Engine/Engine.h"

class UGameInstance;

class UGameEngine : public UEngine
{
    DefineUnrealClass(UGameEngine, Engine)
    DefineUProperty(UGameInstance*, GameInstance)

public:
    static void Hook();

private:
    static inline void (*TickOG)(UGameEngine* This, float DeltaSeconds, bool bIdleMode) = nullptr;
    static void TickHK(UGameEngine* This, float DeltaSeconds, bool bIdleMode);
};
