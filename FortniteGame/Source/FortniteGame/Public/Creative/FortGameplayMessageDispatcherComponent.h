#pragma once

#include "Components/ActorComponent.h"
#include "Containers/Map.h"
#include "GameplayTagContainer.h"
#include "Reflection.h"
#include "UObject/WeakObjectPtr.h"

class AController;
class UFortGameplayReceiverMessageComponent;
class UFortGameplayTriggerMessageComponent;

struct FFortGameplayMessageReceiverList
{
    TArray<UFortGameplayReceiverMessageComponent*> Receivers;
};

struct FFortGameplayMessageTriggerList
{
    TArray<UFortGameplayTriggerMessageComponent*> Triggers;
};

struct FFortTriggeredGameplayMessage
{
    UFortGameplayTriggerMessageComponent* Sender = nullptr;
    FGameplayTag ChannelId;
    TWeakObjectPtr<AController> TriggerInstigator;
};

static_assert(sizeof(FFortTriggeredGameplayMessage) == 0x18);

using FFortGameplayMessageReceiverChannels = TMap<FGameplayTag, FFortGameplayMessageReceiverList>;
using FFortGameplayMessageTriggerChannels = TMap<FGameplayTag, FFortGameplayMessageTriggerList>;

// A TMap member's storage (0x50 bytes, as the engine lays a TSet out): TMap can't be copied, which DefineUProperty's
// setter needs.
struct alignas(8) FFortGameplayMessageChannelsStorage
{
    uint8_t Bytes[sizeof(FFortGameplayMessageReceiverChannels)];
};

// The channels of a Creative island (9.20 on, a component of the island's AFortVolume): which receivers and triggers are
// on each channel, and the messages sent and not yet delivered. 9.20 and 9.21 (CL-6822798, CL-6922310) strip all of its
// server code; see the .cpp.
class UFortGameplayMessageDispatcherComponent : public UActorComponent
{
    DefineUnrealClass(UFortGameplayMessageDispatcherComponent, FortniteGame)

public:
    DefineUProperty(int32_t, NumBoundReceivers)
    DefineUProperty(FFortGameplayMessageChannelsStorage, ReceiverChannelList)
    DefineUProperty(int32_t, NumBoundTriggers)
    DefineUProperty(FFortGameplayMessageChannelsStorage, TriggersChannelList)
    DefineUProperty(TArray<FFortTriggeredGameplayMessage>, EnqueuedMessages)
    DefineUProperty(FFortTriggeredGameplayMessage, ActiveMessage)
    DefineUProperty(FFortGameplayMessageReceiverList, ActiveMessageReceivers)

    FFortGameplayMessageReceiverChannels& GetReceiverChannelList() const { return reinterpret_cast<FFortGameplayMessageReceiverChannels&>(GetMember_ReceiverChannelList()); }
    FFortGameplayMessageTriggerChannels& GetTriggersChannelList() const { return reinterpret_cast<FFortGameplayMessageTriggerChannels&>(GetMember_TriggersChannelList()); }

    void RegisterReceiver(UFortGameplayReceiverMessageComponent* Receiver);
    void UnregisterReceiver(UFortGameplayReceiverMessageComponent* Receiver);
    void RegisterTrigger(UFortGameplayTriggerMessageComponent* Trigger);
    void UnregisterTrigger(UFortGameplayTriggerMessageComponent* Trigger);

    // Queues a message from Sender on ChannelId; the receivers on the channel get it from the next tick on.
    void EnqueueMessage(UFortGameplayTriggerMessageComponent* Sender, const FGameplayTag& ChannelId, AController* TriggerInstigator);

    // Restores the channels where the build strips them (9.20, 9.21). Run once at boot.
    static void HookMessageComponents();

private:
    // Delivers the active message, then the queued ones, to at most CreativeMessageDispatcher.NumReceiversToProcessEachFrame
    // receivers; the rest go on the next tick.
    void ProcessTriggerMessages();
    void ScheduleProcessTriggerMessages();
    // Moves the registered receivers whose ChannelIds changed since they registered onto their channels now.
    void RefreshReceiverChannels();
};
