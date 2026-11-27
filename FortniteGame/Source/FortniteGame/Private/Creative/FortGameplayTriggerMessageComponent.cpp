#include "pch.h"
#include "Creative/FortGameplayTriggerMessageComponent.h"
#include "Creative/FortGameplayMessageDispatcherComponent.h"
#include "GameFramework/Controller.h"
#include "UObject/UnrealType.h"

// BP_Creative_Trigger_C's SendTriggerEvent calls this as the trigger fires (stepped on, damaged, or hit by the music
// sequencer, once CanTrigger passes).
void UFortGameplayTriggerMessageComponent::SendTriggerMessage(AController* TriggerInstigator)
{
#if WITH_SERVER_CODE
    AActor* Owner = GetOwner();
    if (!Owner || Owner->Role != ROLE_Authority)
    {
        return;
    }
    UFortGameplayMessageDispatcherComponent* Dispatcher = GetDispatcher();
    if (!Dispatcher)
    {
        return;
    }
    const FGameplayTagContainer& Channels = ChannelIds;
    for (const FGameplayTag& ChannelId : Channels.GameplayTags)
    {
        if (ChannelId.IsValid())
        {
            Dispatcher->EnqueueMessage(this, ChannelId, TriggerInstigator);
        }
    }
#endif
}

DEFINE_FUNCTION(UFortGameplayTriggerMessageComponent::execSendTriggerMessage)
{
    P_GET_OBJECT(AController, Z_Param_TriggerInstigator);
    P_FINISH;
    P_NATIVE_BEGIN;
    static_cast<UFortGameplayTriggerMessageComponent*>(Context)->SendTriggerMessage(Z_Param_TriggerInstigator);
    P_NATIVE_END;
}

void UFortGameplayTriggerMessageComponent::OnRegister()
{
    OnRegisterOG(this);
#if WITH_SERVER_CODE
    AActor* Owner = GetOwner();
    if (Owner && Owner->Role == ROLE_Authority)
    {
        if (UFortGameplayMessageDispatcherComponent* Dispatcher = GetDispatcher())
        {
            Dispatcher->RegisterTrigger(this);
        }
    }
#endif
}

void UFortGameplayTriggerMessageComponent::EndPlay(EEndPlayReason::Type EndPlayReason)
{
#if WITH_SERVER_CODE
    AActor* Owner = GetOwner();
    if (Owner && Owner->Role == ROLE_Authority)
    {
        if (UFortGameplayMessageDispatcherComponent* Dispatcher = GetDispatcher())
        {
            Dispatcher->UnregisterTrigger(this);
        }
    }
#endif
    EndPlayOG(this, EndPlayReason);
}

void UFortGameplayTriggerMessageComponent::OnRegisterHK(UFortGameplayTriggerMessageComponent* This)
{
    This->OnRegister();
}

void UFortGameplayTriggerMessageComponent::EndPlayHK(UFortGameplayTriggerMessageComponent* This, EEndPlayReason::Type EndPlayReason)
{
    This->EndPlay(EndPlayReason);
}
