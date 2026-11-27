#include "pch.h"
#include "Creative/FortGameplayReceiverMessageComponent.h"
#include "Creative/FortGameplayMessageDispatcherComponent.h"
#include "GameFramework/Controller.h"

void UFortGameplayReceiverMessageComponent::ReceiveMessage(AController* TriggerInstigator)
{
    AController* Parameters = TriggerInstigator;
    const FMulticastScriptDelegate& Received = OnGameplayMessageReceived;
    Received.ProcessMulticastDelegate(&Parameters);
}

void UFortGameplayReceiverMessageComponent::OnRegister()
{
    OnRegisterOG(this);
#if WITH_SERVER_CODE
    AActor* Owner = GetOwner();
    if (Owner && Owner->Role == ROLE_Authority)
    {
        if (UFortGameplayMessageDispatcherComponent* Dispatcher = GetDispatcher())
        {
            Dispatcher->RegisterReceiver(this);
        }
    }
#endif
}

void UFortGameplayReceiverMessageComponent::EndPlay(EEndPlayReason::Type EndPlayReason)
{
#if WITH_SERVER_CODE
    AActor* Owner = GetOwner();
    if (Owner && Owner->Role == ROLE_Authority)
    {
        if (UFortGameplayMessageDispatcherComponent* Dispatcher = GetDispatcher())
        {
            Dispatcher->UnregisterReceiver(this);
        }
    }
#endif
    EndPlayOG(this, EndPlayReason);
}

void UFortGameplayReceiverMessageComponent::OnRegisterHK(UFortGameplayReceiverMessageComponent* This)
{
    This->OnRegister();
}

void UFortGameplayReceiverMessageComponent::EndPlayHK(UFortGameplayReceiverMessageComponent* This, EEndPlayReason::Type EndPlayReason)
{
    This->EndPlay(EndPlayReason);
}
