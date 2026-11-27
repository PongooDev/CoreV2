#pragma once

#include "Creative/FortGameplayMessageComponentBase.h"

class AController;

// A device's listening end (ExplosiveBarrel_GBA_C's ExplodeWhenReceivedOn, the Trigger's EnableWhenReceived): its
// Blueprint binds OnGameplayMessageReceived.
class UFortGameplayReceiverMessageComponent : public UFortGameplayMessageComponentBase
{
    DefineUnrealClass(UFortGameplayReceiverMessageComponent, FortniteGame)

public:
    DefineUProperty(FMulticastScriptDelegate, OnGameplayMessageReceived)

    // A message on one of its channels: OnGameplayMessageReceived(TriggerInstigator).
    void ReceiveMessage(AController* TriggerInstigator);

    void OnRegister();
    void EndPlay(EEndPlayReason::Type EndPlayReason);

    static bool HasOnGameplayMessageReceived() { return Member_OnGameplayMessageReceived.IsAvailable(); }

    static inline void (*OnRegisterOG)(UFortGameplayReceiverMessageComponent* This) = nullptr;
    static inline void (*EndPlayOG)(UFortGameplayReceiverMessageComponent* This, EEndPlayReason::Type EndPlayReason) = nullptr;
    static void OnRegisterHK(UFortGameplayReceiverMessageComponent* This);
    static void EndPlayHK(UFortGameplayReceiverMessageComponent* This, EEndPlayReason::Type EndPlayReason);
};
