#include "pch.h"
#include "OnlineVoiceImpl.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"

#include <atomic>

namespace
{
    bool (*InitOG)(FOnlineVoiceImpl* This) = nullptr;
    void* (*SerializeRemotePacketOG)(FOnlineVoiceImpl* This, void* OutPacket, void* Ar) = nullptr;
    std::atomic<int64_t> RemotePackets = 0;

    // TSharedPtr<FVoicePacket>: the object, then its reference controller.
    struct FSharedPacket
    {
        void* Object;
        void* ReferenceController;
    };
}

void FOnlineVoiceImpl::Hook()
{
    if (!Finder::ResolveOnlineVoice())
    {
        return;
    }
    Hooks::SwapVTable(L"FOnlineVoiceImpl::Init", Natives::FOnlineVoiceImpl_VTable, Natives::FOnlineVoiceImpl_InitIndex, reinterpret_cast<void*>(&InitHK), reinterpret_cast<void**>(&InitOG));
    Hooks::SwapVTable(L"FOnlineVoiceImpl::SerializeRemotePacket", Natives::FOnlineVoiceImpl_VTable, Natives::FOnlineVoiceImpl_SerializeRemotePacketIndex, reinterpret_cast<void*>(&SerializeRemotePacketHK), reinterpret_cast<void**>(&SerializeRemotePacketOG));
}

bool FOnlineVoiceImpl::InitHK(FOnlineVoiceImpl* This)
{
    // A server (dedicated: Core's IsDedicated) only forwards voice and succeeds without a voice engine; a client needs the
    // voice module's capture and Opus encoder ([Voice] bEnabled, a recording device).
    const bool bReady = InitOG(This);
    if (bReady)
    {
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
    if (static_cast<FSharedPacket*>(OutPacket)->Object)
    {
        const int64_t Count = ++RemotePackets;
        if (Count == 1 || Count % 500 == 0)
        {
            Log::Info(L"Voice chat: {} voice packets received", Count);
        }
    }
    return Result;
}
