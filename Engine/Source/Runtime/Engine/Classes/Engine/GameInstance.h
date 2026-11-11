#pragma once

#include "ObjectLayout.h"
#include "Templates/SharedPointer.h"
#include "UObject/CoreOnline.h"
#include "UObject/Object.h"

class FTimerManager;
struct FWorldContext;
class ULocalPlayer;

class UGameInstance : public UObject
{
    DefineUnrealClass(UGameInstance, Engine)

public:
    DefineNativeMember(FWorldContext*, WorldContext, UGameInstance_WorldContext)
    DefineNativeMember(FTimerManager*, TimerManager, UGameInstance_TimerManager)
    DefineUProperty(TArray<ULocalPlayer*>, LocalPlayers)

    bool IsDedicatedServerInstance() const;

    FTimerManager& GetTimerManager() const { return *TimerManager; }

    // Fortnite's UGameInstance::GetGameSessionId (Natives::UGameInstance_GetGameSessionId): the game session's id, or
    // null when there is none or the native was not found.
    TSharedPtr<const FUniqueNetId> GetGameSessionId() const;

    static void Hook();

private:
    static bool IsDedicatedServerInstanceHK(const UGameInstance* This);
};
