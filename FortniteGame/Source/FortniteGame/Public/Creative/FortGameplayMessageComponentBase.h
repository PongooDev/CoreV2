#pragma once

#include "Components/ActorComponent.h"
#include "GameplayTagContainer.h"
#include "Reflection.h"
#include "UObject/ScriptDelegates.h"

class UFortGameplayMessageDispatcherComponent;

// A Creative device's channel end (9.20 on): the Trigger message component sends on its ChannelIds, the Receiver one
// listens on them, and the island volume's UFortGameplayMessageDispatcherComponent passes the messages between them.
// A player's channel pick arrives as AFortPlayerController::ServerUpdateMessageComponents.
class UFortGameplayMessageComponentBase : public UActorComponent
{
    DefineUnrealClass(UFortGameplayMessageComponentBase, FortniteGame)

public:
    DefineUProperty(FMulticastScriptDelegate, OnChannelIdsReplicated)
    DefineUProperty(FGameplayTagContainer, ChannelIds)

    // Epic's SetChannelId (a UFunction by 21.40; 9.20/9.21's ServerUpdateMessageComponents_Implementation calls the
    // shared empty function in its place).
    void SetChannelId(const FGameplayTagContainer& NewChannelIds);

    // The dispatcher of the island the owner is in, else null.
    UFortGameplayMessageDispatcherComponent* GetDispatcher() const;

    static bool HasChannelIds() { return Member_ChannelIds.IsAvailable(); }

    static void SetChannelIdCall(UFortGameplayMessageComponentBase* This, const FGameplayTagContainer& NewChannelIds);
};
