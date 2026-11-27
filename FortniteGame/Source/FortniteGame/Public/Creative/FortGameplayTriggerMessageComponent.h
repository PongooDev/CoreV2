#pragma once

#include "Creative/FortGameplayMessageComponentBase.h"
#include "UObject/Stack.h"

class AController;

// A device's sending end (BP_Creative_Trigger_C's ChannelTrigger): SendTriggerMessage sends on each of its channels.
class UFortGameplayTriggerMessageComponent : public UFortGameplayMessageComponentBase
{
    DefineUnrealClass(UFortGameplayTriggerMessageComponent, FortniteGame)
    DefineUFunction(SendTriggerMessage)

public:
    void SendTriggerMessage(AController* TriggerInstigator);

    void OnRegister();
    void EndPlay(EEndPlayReason::Type EndPlayReason);

    static inline void (*OnRegisterOG)(UFortGameplayTriggerMessageComponent* This) = nullptr;
    static inline void (*EndPlayOG)(UFortGameplayTriggerMessageComponent* This, EEndPlayReason::Type EndPlayReason) = nullptr;
    static void OnRegisterHK(UFortGameplayTriggerMessageComponent* This);
    static void EndPlayHK(UFortGameplayTriggerMessageComponent* This, EEndPlayReason::Type EndPlayReason);

    DECLARE_FUNCTION(execSendTriggerMessage);
};
