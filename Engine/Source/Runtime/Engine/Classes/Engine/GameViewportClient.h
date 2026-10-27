#pragma once

#include "Engine/Console.h"
#include "UObject/Object.h"

class UEngine;
class UGameInstance;
class ULocalPlayer;

class UGameViewportClient : public UObject
{
    DefineUnrealClass(UGameViewportClient, Engine)

public:
    DefineUProperty(UConsole*, ViewportConsole)
    DefineUProperty(UGameInstance*, GameInstance)

    UEngine* GetOuterUEngine() const;
    ULocalPlayer* SetupInitialLocalPlayer(FString& OutError);

    static void Hook();

private:
    static inline ULocalPlayer* (*SetupInitialLocalPlayerOG)(UGameViewportClient* This, FString& OutError) = nullptr;
    static ULocalPlayer* SetupInitialLocalPlayerHK(UGameViewportClient* This, FString& OutError);
};
