#include "pch.h"
#include "OnlineVoiceImpl.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "GameFramework/OnlineReplStructs.h"

#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>

namespace
{
    bool (*InitOG)(FOnlineVoiceImpl* This) = nullptr;
    void* (*SerializeRemotePacketOG)(FOnlineVoiceImpl* This, void* OutPacket, void* Ar) = nullptr;
    void* (*GetLocalPacketOG)(FOnlineVoiceImpl* This, void* OutPacket, uint32_t LocalUserNum) = nullptr;
    void (*TickOG)(FOnlineVoiceImpl* This, float DeltaTime) = nullptr;
    std::atomic<int64_t> RemotePackets = 0;
    std::atomic<int64_t> LocalPacketRequests = 0;
    std::atomic<int64_t> LocalPackets = 0;
    // The voice interface that started with a voice engine (the client's).
    FOnlineVoiceImpl* LocalVoice = nullptr;

    // TSharedPtr<FVoicePacket>: the object, then its reference controller.
    struct FSharedPacket
    {
        void* Object;
        void* ReferenceController;
    };

    // A player shows as talking this long after their last voice packet. A talking player's client sends one with each
    // net update while push to talk is held and the microphone records something, so the icon goes idle a moment after
    // they let go (or, if the voice capture drops silence, between sentences).
    constexpr double TalkingHoldSeconds = 0.35;
    // Client, on the game thread: when each remote player's voice last arrived, by unique id (lowercase: Vivox's talking
    // updates matched the ids ignoring case), and when the microphone last recorded.
    std::unordered_map<std::wstring, double> RemoteVoiceTimes;
    double LocalVoiceTime = -TalkingHoldSeconds;

    double Now()
    {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // FUniqueNetId's IsValid and ToString slots come from the Battle Royale lobby's finder (Finder::ResolveAthenaMenuHero).
    bool CanReadIds()
    {
        return Natives::FUniqueNetId_IsValidIndex >= 0 && Natives::FUniqueNetId_ToStringIndex >= 0;
    }

    std::wstring TalkerKey(const FUniqueNetId& Id)
    {
        return Id.ToString().ToLower().ToStdString();
    }
}

void FOnlineVoiceImpl::Hook()
{
    if (!Finder::ResolveOnlineVoice())
    {
        return;
    }
    Hooks::SwapVTable(L"FOnlineVoiceImpl::Init", Natives::FOnlineVoiceImpl_VTable, Natives::FOnlineVoiceImpl_InitIndex, reinterpret_cast<void*>(&InitHK), reinterpret_cast<void**>(&InitOG));
    Hooks::SwapVTable(L"FOnlineVoiceImpl::SerializeRemotePacket", Natives::FOnlineVoiceImpl_VTable, Natives::FOnlineVoiceImpl_SerializeRemotePacketIndex, reinterpret_cast<void*>(&SerializeRemotePacketHK), reinterpret_cast<void**>(&SerializeRemotePacketOG));
    if (Natives::FOnlineVoiceImpl_GetLocalPacketIndex >= 0)
    {
        Hooks::SwapVTable(L"FOnlineVoiceImpl::GetLocalPacket", Natives::FOnlineVoiceImpl_VTable, Natives::FOnlineVoiceImpl_GetLocalPacketIndex, reinterpret_cast<void*>(&GetLocalPacketHK), reinterpret_cast<void**>(&GetLocalPacketOG));
    }
#if !UE_SERVER
    if (Natives::FOnlineVoiceImpl_TickIndex >= 0)
    {
        Hooks::SwapVTable(L"FOnlineVoiceImpl::Tick", Natives::FOnlineVoiceImpl_VTable, Natives::FOnlineVoiceImpl_TickIndex, reinterpret_cast<void*>(&TickHK), reinterpret_cast<void**>(&TickOG));
    }
#endif
}

bool FOnlineVoiceImpl::InitHK(FOnlineVoiceImpl* This)
{
    // A server (dedicated: Core's IsDedicated) only forwards voice and succeeds without a voice engine; a client needs the
    // voice module's capture and Opus encoder ([Voice] bEnabled, a recording device).
    const bool bReady = InitOG(This);
    if (bReady)
    {
        LocalVoice = This;
        Log::Info(L"Voice chat: voice interface ready");
    }
    else
    {
        Log::Warning(L"Voice chat: voice interface failed to start (no recording device, or the voice module is off)");
    }
    return bReady;
}

void* FOnlineVoiceImpl::SerializeRemotePacketHK(FOnlineVoiceImpl* This, void* OutPacket, void* Ar)
{
    void* Result = SerializeRemotePacketOG(This, OutPacket, Ar);
    void* const Packet = static_cast<FSharedPacket*>(OutPacket)->Object;
    if (Packet)
    {
        const int64_t Count = ++RemotePackets;
        if (Count == 1 || Count % 500 == 0)
        {
            Log::Info(L"Voice chat: {} voice packets received", Count);
        }
    }
#if !UE_SERVER
    // The packet carries its talker's unique id, which the server forwards unchanged.
    if (Packet && Natives::FVoicePacket_GetSenderIndex >= 0 && CanReadIds())
    {
        TSharedPtr<const FUniqueNetId> Sender;
        CallVirtual<void*>(Packet, Natives::FVoicePacket_GetSenderIndex, &Sender);
        if (Sender.IsValid() && Sender->IsValid())
        {
            const auto [Entry, bFirst] = RemoteVoiceTimes.insert_or_assign(TalkerKey(*Sender), Now());
            if (bFirst)
            {
                Log::Info(L"Voice chat: voice from {} received", Entry->first);
            }
        }
    }
#endif
    return Result;
}

void* FOnlineVoiceImpl::GetLocalPacketHK(FOnlineVoiceImpl* This, void* OutPacket, uint32_t LocalUserNum)
{
    // The client's net driver takes the microphone's packets from here every frame to send to the server: no requests
    // means it never asks (no voice channel, or no local talkers), requests without packets mean the microphone records
    // nothing.
    void* Result = GetLocalPacketOG(This, OutPacket, LocalUserNum);
    const int64_t Requests = ++LocalPacketRequests;
    if (Requests == 1)
    {
        Log::Info(L"Voice chat: the game asks for the microphone's voice packets");
    }
    if (static_cast<FSharedPacket*>(OutPacket)->Object)
    {
        LocalVoiceTime = Now();
        const int64_t Count = ++LocalPackets;
        if (Count == 1 || Count % 500 == 0)
        {
            Log::Info(L"Voice chat: {} voice packets recorded to send", Count);
        }
    }
    return Result;
}

void FOnlineVoiceImpl::RegisterLocalTalker(uint32_t LocalUserNum)
{
    // Starting the microphone (FVoiceEngineImpl::StartLocalVoiceProcessing, 1.7.2 0x392D1F0) fails with "Device is
    // currently owned by another user" until the player is registered as a local talker, which the session interface
    // does when it creates or joins a session; Battle Royale's matchmaking never did, so pressing push to talk sent
    // nothing (Core-Backend #54: "talking" on both clients, no voice packets on the server). Registering an already
    // registered talker does nothing.
    static bool bRegistered = false;
    if (bRegistered || !LocalVoice || Natives::FOnlineVoiceImpl_RegisterLocalTalkerIndex < 0)
    {
        return;
    }
    using FRegisterLocalTalker = bool (*)(FOnlineVoiceImpl*, uint32_t);
    void** VTable = *reinterpret_cast<void***>(LocalVoice);
    bRegistered = reinterpret_cast<FRegisterLocalTalker>(VTable[Natives::FOnlineVoiceImpl_RegisterLocalTalkerIndex])(LocalVoice, LocalUserNum);
    Log::Info(L"Voice chat: local talker {} {}", LocalUserNum, bRegistered ? L"registered" : L"failed to register (no voice engine, or no account id yet)");
}

void FOnlineVoiceImpl::TickHK(FOnlineVoiceImpl* This, float DeltaTime)
{
    TickOG(This, DeltaTime);
    static bool bTicked = false;
    if (!bTicked)
    {
        bTicked = true;
        Log::Info(L"Voice chat: voice interface ticking");
    }
    if (OnTick)
    {
        OnTick();
    }
}

bool FOnlineVoiceImpl::IsRemotePlayerTalking(const FUniqueNetIdRepl& PlayerId)
{
    // The engine's own talking state (FOnlineVoiceImpl's RemoteTalkers, OnPlayerTalkingStateChanged) covers only talkers
    // registered with RegisterRemoteTalker (1.7.2 0x39294E0), which the session interface does for its sessions'
    // players when there is a session; Battle Royale's HUD never read it anyway (Vivox set its talking state). The
    // packets' arrival times stand in for it.
    if (RemoteVoiceTimes.empty() || !CanReadIds() || !PlayerId.IsValid())
    {
        return false;
    }
    const auto Found = RemoteVoiceTimes.find(TalkerKey(*PlayerId));
    return Found != RemoteVoiceTimes.end() && Now() - Found->second < TalkingHoldSeconds;
}

bool FOnlineVoiceImpl::IsLocalPlayerTalking()
{
    return Now() - LocalVoiceTime < TalkingHoldSeconds;
}
