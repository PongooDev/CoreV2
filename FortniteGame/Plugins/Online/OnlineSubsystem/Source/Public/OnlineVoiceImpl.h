#pragma once

#include <cstdint>

// The engine's voice interface (OnlineSubsystemUtils' FOnlineVoiceImpl), on clients and servers: Core logs whether it
// started and counts the voice packets that reach it, so a voice chat test shows where the voice stops.
class FOnlineVoiceImpl
{
public:
    static void Hook();
    // Client: registers the local player as a talker, which starting the microphone needs; once is enough.
    static void RegisterLocalTalker(uint32_t LocalUserNum);

private:
    static bool InitHK(FOnlineVoiceImpl* This);
    static void* SerializeRemotePacketHK(FOnlineVoiceImpl* This, void* OutPacket, void* Ar);
    static void* GetLocalPacketHK(FOnlineVoiceImpl* This, void* OutPacket, uint32_t LocalUserNum);
};
