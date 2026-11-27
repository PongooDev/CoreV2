#include "pch.h"
#include "Creative/FortGameplayMessageComponentBase.h"
#include "Creative/FortGameplayMessageDispatcherComponent.h"
#include "Creative/FortGameplayReceiverMessageComponent.h"
#include "Creative/FortVolume.h"
#include "Templates/Casts.h"

void UFortGameplayMessageComponentBase::SetChannelId(const FGameplayTagContainer& NewChannelIds)
{
#if WITH_SERVER_CODE
    AActor* Owner = GetOwner();
    if (!Owner || Owner->Role != ROLE_Authority)
    {
        return;
    }
    ChannelIds = NewChannelIds;
    Owner->ForceNetUpdate();
    // A receiver moves to its new channels (21.40 tells the dispatcher through OnChannelIdUpdated); a trigger sends on its
    // ChannelIds as they are when it sends.
    if (UFortGameplayReceiverMessageComponent* Receiver = Cast<UFortGameplayReceiverMessageComponent>(this))
    {
        if (UFortGameplayMessageDispatcherComponent* Dispatcher = GetDispatcher())
        {
            Dispatcher->UnregisterReceiver(Receiver);
            Dispatcher->RegisterReceiver(Receiver);
        }
    }
#endif
}

void UFortGameplayMessageComponentBase::SetChannelIdCall(UFortGameplayMessageComponentBase* This, const FGameplayTagContainer& NewChannelIds)
{
    if (This)
    {
        This->SetChannelId(NewChannelIds);
    }
}

UFortGameplayMessageDispatcherComponent* UFortGameplayMessageComponentBase::GetDispatcher() const
{
    AActor* Owner = GetOwner();
    AFortVolumeManager* VolumeManager = Owner ? AFortVolumeManager::Get(Owner) : nullptr;
    AFortVolume* Volume = VolumeManager ? VolumeManager->GetVolumeForActor(Owner) : nullptr;
    if (!Volume)
    {
        return nullptr;
    }
    // AFortMinigame's state changes look the dispatcher up on the minigame's Volume the same way (9.21 0x1644FE8).
    const TArray<UActorComponent*> Dispatchers = Volume->GetComponentsByClass(UFortGameplayMessageDispatcherComponent::StaticClass());
    return Dispatchers.Num() > 0 ? static_cast<UFortGameplayMessageDispatcherComponent*>(Dispatchers[0]) : nullptr;
}
