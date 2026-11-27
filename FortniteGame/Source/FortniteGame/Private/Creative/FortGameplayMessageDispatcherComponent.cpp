#include "pch.h"
#include "Creative/FortGameplayMessageDispatcherComponent.h"
#include "Creative/FortGameplayMessageComponentBase.h"
#include "Creative/FortGameplayReceiverMessageComponent.h"
#include "Creative/FortGameplayTriggerMessageComponent.h"
#include "Engine/World.h"
#include "Finder.h"
#include "GameFramework/Controller.h"
#include "HAL/IConsoleManager.h"
#include "Hooks.h"
#include "Log.h"
#include "Logging/LogMacros.h"
#include "Natives.h"
#include "Player/FortPlayerController.h"
#include "TimerManager.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY_STATIC(LogFortCreative, Log, All);

namespace
{
    // The game's CreativeMessageDispatcher.* console variables (registered in 9.20/9.21 with the rest of the dispatcher
    // stripped); Fallback where one isn't found.
    int32_t DispatcherSetting(const wchar_t* Name, int32_t Fallback)
    {
        IConsoleVariable* Variable = IConsoleManager::Get().FindConsoleVariable(Name);
        const int32_t Value = Variable ? Variable->GetInt() : 0;
        return Value > 0 ? Value : Fallback;
    }
}

void UFortGameplayMessageDispatcherComponent::RegisterReceiver(UFortGameplayReceiverMessageComponent* Receiver)
{
    if (!Receiver)
    {
        return;
    }
    const int32_t MaxBoundReceivers = DispatcherSetting(L"CreativeMessageDispatcher.MaxBoundReceivers", 1000);
    const int32_t MaxReceiversPerChannel = DispatcherSetting(L"CreativeMessageDispatcher.MaxReceiversPerChannel", 1000);
    FFortGameplayMessageReceiverChannels& Channels = GetReceiverChannelList();
    for (auto& Channel : Channels)
    {
        if (Channel.Value.Receivers.Contains(Receiver))
        {
            return;
        }
    }
    const FGameplayTagContainer& ReceiverChannels = Receiver->ChannelIds;
    // A receiver without a channel yet waits under the empty tag, so RefreshReceiverChannels can still move it.
    TArray<FGameplayTag> Keys = ReceiverChannels.GameplayTags;
    if (Keys.Num() == 0)
    {
        Keys.Add(FGameplayTag());
    }
    bool bBound = false;
    for (const FGameplayTag& ChannelId : Keys)
    {
        TArray<UFortGameplayReceiverMessageComponent*>& Receivers = Channels.FindOrAdd(ChannelId).Receivers;
        if (NumBoundReceivers >= MaxBoundReceivers || (ChannelId.IsValid() && Receivers.Num() >= MaxReceiversPerChannel))
        {
            UE_LOG(LogFortCreative, Warning, TEXT("Message dispatcher %s: %s can't listen on %s (%d receivers bound, %d on the channel)"), *GetName(),
                *Receiver->GetName(), *ChannelId.TagName.ToString(), static_cast<int32_t>(NumBoundReceivers), Receivers.Num());
            continue;
        }
        Receivers.Add(Receiver);
        bBound = true;
    }
    if (bBound)
    {
        NumBoundReceivers = NumBoundReceivers + 1;
    }
}

void UFortGameplayMessageDispatcherComponent::UnregisterReceiver(UFortGameplayReceiverMessageComponent* Receiver)
{
    bool bWasBound = false;
    FFortGameplayMessageReceiverChannels& Channels = GetReceiverChannelList();
    for (auto& Channel : Channels)
    {
        bWasBound |= Channel.Value.Receivers.RemoveSingle(Receiver) > 0;
    }
    if (bWasBound && NumBoundReceivers > 0)
    {
        NumBoundReceivers = NumBoundReceivers - 1;
    }
}

void UFortGameplayMessageDispatcherComponent::RegisterTrigger(UFortGameplayTriggerMessageComponent* Trigger)
{
    if (!Trigger)
    {
        return;
    }
    const int32_t MaxTriggers = DispatcherSetting(L"CreativeMessageDispatcher.MaxTriggers", 1000);
    FFortGameplayMessageTriggerChannels& Channels = GetTriggersChannelList();
    for (auto& Channel : Channels)
    {
        if (Channel.Value.Triggers.Contains(Trigger))
        {
            return;
        }
    }
    if (NumBoundTriggers >= MaxTriggers)
    {
        UE_LOG(LogFortCreative, Warning, TEXT("Message dispatcher %s: %s can't send (%d triggers bound)"), *GetName(), *Trigger->GetName(), static_cast<int32_t>(NumBoundTriggers));
        return;
    }
    // A trigger sends on its ChannelIds as they are when it fires, so it is only counted here, under its channels now.
    const FGameplayTagContainer& TriggerChannels = Trigger->ChannelIds;
    Channels.FindOrAdd(TriggerChannels.GameplayTags.Num() > 0 ? TriggerChannels.GameplayTags[0] : FGameplayTag()).Triggers.Add(Trigger);
    NumBoundTriggers = NumBoundTriggers + 1;
}

void UFortGameplayMessageDispatcherComponent::UnregisterTrigger(UFortGameplayTriggerMessageComponent* Trigger)
{
    FFortGameplayMessageTriggerChannels& Channels = GetTriggersChannelList();
    for (auto& Channel : Channels)
    {
        if (Channel.Value.Triggers.RemoveSingle(Trigger) > 0)
        {
            if (NumBoundTriggers > 0)
            {
                NumBoundTriggers = NumBoundTriggers - 1;
            }
            return;
        }
    }
}

// A level save applies a device's saved ChannelIds after its components registered, and a receiver's channels can change
// with no call the server sees, so the receivers are put on their current channels before a message goes out. Core's:
// 9.20/9.21 has no other trace of how Epic kept the lists current (21.40's dispatcher defers registrations and hears of
// loaded channels through OnChannelIdLoaded).
void UFortGameplayMessageDispatcherComponent::RefreshReceiverChannels()
{
    FFortGameplayMessageReceiverChannels& Channels = GetReceiverChannelList();
    TArray<UFortGameplayReceiverMessageComponent*> Bound;
    for (auto& Channel : Channels)
    {
        TArray<UFortGameplayReceiverMessageComponent*>& Receivers = Channel.Value.Receivers;
        for (int32_t Index = Receivers.Num() - 1; Index >= 0; --Index)
        {
            UFortGameplayReceiverMessageComponent* Receiver = Receivers[Index];
            if (!Receiver || Receiver->IsPendingKill())
            {
                Receivers.RemoveAt(Index);
            }
            else if (!Bound.Contains(Receiver))
            {
                Bound.Add(Receiver);
            }
        }
    }
    NumBoundReceivers = Bound.Num();
    for (UFortGameplayReceiverMessageComponent* Receiver : Bound)
    {
        // Where it is listed against where its ChannelIds (or, without any, the empty tag) say it should be.
        const FGameplayTagContainer& ReceiverChannels = Receiver->ChannelIds;
        bool bCurrent = true;
        for (auto& Channel : Channels)
        {
            const bool bListed = Channel.Value.Receivers.Contains(Receiver);
            const bool bWanted = Channel.Key.IsValid() ? ReceiverChannels.GameplayTags.Contains(Channel.Key) : ReceiverChannels.GameplayTags.Num() == 0;
            bCurrent &= bListed == bWanted;
        }
        for (const FGameplayTag& ChannelId : ReceiverChannels.GameplayTags)
        {
            bCurrent &= !ChannelId.IsValid() || Channels.Contains(ChannelId);
        }
        if (!bCurrent)
        {
            UnregisterReceiver(Receiver);
            RegisterReceiver(Receiver);
        }
    }
}

void UFortGameplayMessageDispatcherComponent::EnqueueMessage(UFortGameplayTriggerMessageComponent* Sender, const FGameplayTag& ChannelId, AController* TriggerInstigator)
{
    TArray<FFortTriggeredGameplayMessage>& Messages = EnqueuedMessages;
    const int32_t MaxEnqueuedMessages = DispatcherSetting(L"CreativeMessageDispatcher.MaxEnqueuedMessages", 1000);
    if (Messages.Num() >= MaxEnqueuedMessages)
    {
        UE_LOG(LogFortCreative, Warning, TEXT("Message dispatcher %s: %s's message on %s dropped (%d messages queued)"), *GetName(), Sender ? *Sender->GetName() : TEXT("None"),
            *ChannelId.TagName.ToString(), Messages.Num());
        return;
    }
    RefreshReceiverChannels();
    const bool bWasIdle = !ActiveMessage.Sender && Messages.Num() == 0;
    FFortTriggeredGameplayMessage Message;
    Message.Sender = Sender;
    Message.ChannelId = ChannelId;
    Message.TriggerInstigator = TriggerInstigator;
    Messages.Add(Message);
    const FFortGameplayMessageReceiverChannels& Channels = GetReceiverChannelList();
    const FFortGameplayMessageReceiverList* Listeners = Channels.Find(ChannelId);
    UE_LOG(LogFortCreative, Log, TEXT("Message dispatcher %s: %s sends on %s to %d receiver(s)"), *GetName(), Sender ? *Sender->GetName() : TEXT("None"),
        *ChannelId.TagName.ToString(), Listeners ? Listeners->Receivers.Num() : 0);
    if (bWasIdle)
    {
        ScheduleProcessTriggerMessages();
    }
}

void UFortGameplayMessageDispatcherComponent::ScheduleProcessTriggerMessages()
{
    AActor* Owner = GetOwner();
    UWorld* World = Owner ? Owner->GetWorld() : nullptr;
    if (!World)
    {
        return;
    }
    const TWeakObjectPtr<UFortGameplayMessageDispatcherComponent> WeakThis(this);
    World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateLambda([WeakThis]
    {
        if (UFortGameplayMessageDispatcherComponent* Dispatcher = WeakThis.Get())
        {
            Dispatcher->ProcessTriggerMessages();
        }
    }));
}

void UFortGameplayMessageDispatcherComponent::ProcessTriggerMessages()
{
    int32_t Budget = DispatcherSetting(L"CreativeMessageDispatcher.NumReceiversToProcessEachFrame", 100);
    TArray<FFortTriggeredGameplayMessage>& Messages = EnqueuedMessages;
    FFortTriggeredGameplayMessage& Active = ActiveMessage;
    TArray<UFortGameplayReceiverMessageComponent*>& Pending = ActiveMessageReceivers.Receivers;
    while (Budget > 0)
    {
        if (!Active.Sender)
        {
            if (Messages.Num() == 0)
            {
                break;
            }
            Active = Messages[0];
            Messages.RemoveAt(0);
            const FFortGameplayMessageReceiverChannels& Channels = GetReceiverChannelList();
            const FFortGameplayMessageReceiverList* Listeners = Channels.Find(Active.ChannelId);
            Pending = Listeners ? Listeners->Receivers : TArray<UFortGameplayReceiverMessageComponent*>();
        }
        while (Budget > 0 && Pending.Num() > 0)
        {
            UFortGameplayReceiverMessageComponent* Receiver = Pending[0];
            Pending.RemoveAt(0);
            --Budget;
            if (Receiver && !Receiver->IsPendingKill())
            {
                Receiver->ReceiveMessage(Active.TriggerInstigator.Get());
            }
        }
        if (Pending.Num() == 0)
        {
            Active = FFortTriggeredGameplayMessage();
        }
    }
    if (Active.Sender || Messages.Num() > 0)
    {
        ScheduleProcessTriggerMessages();
    }
}

// 9.20 and 9.21 (CL-6822798, CL-6922310) strip the Creative channels' server code:
// - UFortGameplayTriggerMessageComponent::execSendTriggerMessage (9.21 0x1ABC560) steps the controller and calls the
//   shared empty function, so a Trigger device (BP_Creative_Trigger_C's SendTriggerEvent) sends nothing;
// - the Trigger and Receiver components' OnRegister (UActorComponent slot 0x52) and EndPlay (0x60) overrides are only a
//   jmp to UActorComponent's (9.21 0x6EBEA0, 0x12978F0), so nothing registers on the island's dispatcher;
// - AFortPlayerController::ServerUpdateMessageComponents_Implementation (9.21 0x18AC370) calls the shared empty
//   function where it sets each component's channels (SetChannelId), so a channel picked in the device's menu isn't kept;
// - the dispatcher has no server code at all (its vtable overrides only the destructor; AFortMinigame's calls of it,
//   0x1624B2B and 0x164500B, call the shared empty function). Those minigame calls aren't restored.
// The game's own triggers are used: the exec, the vtable slots and the _Implementation's call. 9.10 and older have no
// message components.
void UFortGameplayMessageDispatcherComponent::HookMessageComponents()
{
#if WITH_SERVER_CODE
    UClass* const Class = StaticClass();
    UClass* const TriggerClass = Class ? UFortGameplayTriggerMessageComponent::StaticClass() : nullptr;
    UClass* const ReceiverClass = TriggerClass ? UFortGameplayReceiverMessageComponent::StaticClass() : nullptr;
    UFunction* const SendTriggerMessage = ReceiverClass ? UFortGameplayTriggerMessageComponent::FindStaticFunction_SendTriggerMessage() : nullptr;
    if (!SendTriggerMessage)
    {
        return;
    }
    uint8_t* const NativeCall = Finder::ImplementationCall(SendTriggerMessage);
    if (!NativeCall || !Finder::IsStubFunction(Finder::RelativeTarget(reinterpret_cast<uintptr_t>(NativeCall), 1, 5)))
    {
        Log::Info(L"Finder: UFortGameplayTriggerMessageComponent::SendTriggerMessage is native in this build");
        return;
    }
    if (!Member_ReceiverChannelList.IsAvailable() || !Member_TriggersChannelList.IsAvailable() || !Member_EnqueuedMessages.IsAvailable() || !Member_ActiveMessage.IsAvailable()
        || !Member_ActiveMessageReceivers.IsAvailable() || !Member_NumBoundReceivers.IsAvailable() || !Member_NumBoundTriggers.IsAvailable()
        || !UFortGameplayMessageComponentBase::HasChannelIds() || !UFortGameplayReceiverMessageComponent::HasOnGameplayMessageReceived())
    {
        Log::Error(L"Finder: UFortGameplayTriggerMessageComponent::SendTriggerMessage is stripped, but a member of the message components or the dispatcher is missing, so Creative channels don't work");
        return;
    }

    // OnRegister and EndPlay: the two UActorComponent slots both components override with a jmp to UActorComponent's.
    UObject* const ComponentDefault = UActorComponent::StaticClass()->GetDefaultObject(false);
    void** const ComponentVTable = ComponentDefault ? *reinterpret_cast<void***>(ComponentDefault) : nullptr;
    const auto IsCode = [](const void* Address)
    {
        return reinterpret_cast<uintptr_t>(Address) >= Finder::TextBegin() && reinterpret_cast<uintptr_t>(Address) < Finder::TextEnd();
    };
    const auto ForwardingSlots = [&](UClass* ComponentClass)
    {
        TArray<int32_t> Slots;
        UObject* const Default = ComponentClass->GetDefaultObject(false);
        void** const VTable = Default ? *reinterpret_cast<void***>(Default) : nullptr;
        for (int32_t Index = 1; ComponentVTable && VTable && Index < 0x100 && IsCode(ComponentVTable[Index]); ++Index)
        {
            const uint8_t* const Code = reinterpret_cast<const uint8_t*>(VTable[Index]);
            if (VTable[Index] != ComponentVTable[Index] && IsCode(Code) && Code[0] == 0xE9
                && reinterpret_cast<uintptr_t>(Code) + 5 + *reinterpret_cast<const int32_t*>(Code + 1) == reinterpret_cast<uintptr_t>(ComponentVTable[Index]))
            {
                Slots.Add(Index);
            }
        }
        return Slots;
    };
    const TArray<int32_t> TriggerSlots = ForwardingSlots(TriggerClass);
    const TArray<int32_t> ReceiverSlots = ForwardingSlots(ReceiverClass);
    const bool bSlotsFound = TriggerSlots.Num() == 2 && ReceiverSlots.Num() == 2 && TriggerSlots[0] == ReceiverSlots[0] && TriggerSlots[1] == ReceiverSlots[1];

    // ServerUpdateMessageComponents_Implementation's one call of the shared empty function (SetChannelId).
    UFunction* const UpdateMessageComponents = FindObject<UFunction>(nullptr, L"/Script/FortniteGame.FortPlayerController:ServerUpdateMessageComponents");
    const int32_t UpdateIndex = UpdateMessageComponents ? Finder::ImplementationIndex(UpdateMessageComponents) : -1;
    UObject* const ControllerDefault = UpdateIndex >= 0 && AFortPlayerController::StaticClass() ? AFortPlayerController::StaticClass()->GetDefaultObject(false) : nullptr;
    const uintptr_t UpdateImplementation = ControllerDefault ? reinterpret_cast<uintptr_t>((*reinterpret_cast<void***>(ControllerDefault))[UpdateIndex]) : 0;
    uintptr_t SetChannelIdCall = 0;
    int32_t EmptyCalls = 0;
    for (const auto& [Site, Target] : UpdateImplementation && Natives::SharedEmptyFunction ? Finder::DecodedDirectCalls(UpdateImplementation) : std::vector<std::pair<uintptr_t, uintptr_t>>())
    {
        if (Target == reinterpret_cast<uintptr_t>(Natives::SharedEmptyFunction))
        {
            SetChannelIdCall = Site;
            ++EmptyCalls;
        }
    }

    if (!bSlotsFound || EmptyCalls != 1)
    {
        Log::Error(L"Finder: UFortGameplayTriggerMessageComponent::SendTriggerMessage is stripped, but its restoration is missing a part (OnRegister/EndPlay slots: trigger {}, receiver {}; "
            L"ServerUpdateMessageComponents_Implementation's SetChannelId call: {} candidates), so Creative channels don't work",
            TriggerSlots.Num(), ReceiverSlots.Num(), EmptyCalls);
        return;
    }

    const uintptr_t GameExec = reinterpret_cast<uintptr_t>(SendTriggerMessage->GetNativeFunc());
    SendTriggerMessage->SetNativeFunc(&UFortGameplayTriggerMessageComponent::execSendTriggerMessage);
    const int32_t OnRegisterIndex = TriggerSlots[0];
    const int32_t EndPlayIndex = TriggerSlots[1];
    void** const TriggerVTable = *reinterpret_cast<void***>(TriggerClass->GetDefaultObject(false));
    void** const ReceiverVTable = *reinterpret_cast<void***>(ReceiverClass->GetDefaultObject(false));
    UFortGameplayTriggerMessageComponent::OnRegisterOG = reinterpret_cast<decltype(UFortGameplayTriggerMessageComponent::OnRegisterOG)>(TriggerVTable[OnRegisterIndex]);
    UFortGameplayTriggerMessageComponent::EndPlayOG = reinterpret_cast<decltype(UFortGameplayTriggerMessageComponent::EndPlayOG)>(TriggerVTable[EndPlayIndex]);
    UFortGameplayReceiverMessageComponent::OnRegisterOG = reinterpret_cast<decltype(UFortGameplayReceiverMessageComponent::OnRegisterOG)>(ReceiverVTable[OnRegisterIndex]);
    UFortGameplayReceiverMessageComponent::EndPlayOG = reinterpret_cast<decltype(UFortGameplayReceiverMessageComponent::EndPlayOG)>(ReceiverVTable[EndPlayIndex]);
    int32_t Swapped = 0;
    for (TObjectIterator<UClass> It; It; ++It)
    {
        const bool bTrigger = It->IsChildOf(TriggerClass);
        const bool bReceiver = !bTrigger && It->IsChildOf(ReceiverClass);
        UObject* const ClassDefault = bTrigger || bReceiver ? It->GetDefaultObject(false) : nullptr;
        void** const ClassVTable = ClassDefault ? *reinterpret_cast<void***>(ClassDefault) : nullptr;
        if (!ClassVTable)
        {
            continue;
        }
        if (bTrigger && ClassVTable[OnRegisterIndex] == reinterpret_cast<void*>(UFortGameplayTriggerMessageComponent::OnRegisterOG))
        {
            Hooks::SwapVTable(L"UFortGameplayTriggerMessageComponent::OnRegister", ClassVTable, OnRegisterIndex, reinterpret_cast<void*>(&UFortGameplayTriggerMessageComponent::OnRegisterHK));
            Hooks::SwapVTable(L"UFortGameplayTriggerMessageComponent::EndPlay", ClassVTable, EndPlayIndex, reinterpret_cast<void*>(&UFortGameplayTriggerMessageComponent::EndPlayHK));
            ++Swapped;
        }
        else if (bReceiver && ClassVTable[OnRegisterIndex] == reinterpret_cast<void*>(UFortGameplayReceiverMessageComponent::OnRegisterOG))
        {
            Hooks::SwapVTable(L"UFortGameplayReceiverMessageComponent::OnRegister", ClassVTable, OnRegisterIndex, reinterpret_cast<void*>(&UFortGameplayReceiverMessageComponent::OnRegisterHK));
            Hooks::SwapVTable(L"UFortGameplayReceiverMessageComponent::EndPlay", ClassVTable, EndPlayIndex, reinterpret_cast<void*>(&UFortGameplayReceiverMessageComponent::EndPlayHK));
            ++Swapped;
        }
    }
    Hooks::RedirectCall(L"AFortPlayerController::ServerUpdateMessageComponents_Implementation -> UFortGameplayMessageComponentBase::SetChannelId",
        reinterpret_cast<uint8_t*>(SetChannelIdCall), reinterpret_cast<void*>(&UFortGameplayMessageComponentBase::SetChannelIdCall));
    const uintptr_t Base = Finder::ImageBase();
    Log::Info(L"Finder: Creative's channels are stripped (SendTriggerMessage's exec +0x{:X}), restored: the message components register on their island's "
        L"dispatcher (OnRegister slot 0x{:X}, EndPlay 0x{:X}, {} classes), ServerUpdateMessageComponents_Implementation's SetChannelId call +0x{:X}",
        GameExec - Base, OnRegisterIndex, EndPlayIndex, Swapped, SetChannelIdCall - Base);
#endif
}
