#pragma once

// The engine's voice interface (OnlineSubsystemUtils' FOnlineVoiceImpl), on clients and servers: Core logs whether it
// started and counts the voice packets that reach it, so a voice chat test shows where the voice stops.
class FOnlineVoiceImpl
{
public:
    static void Hook();

private:
    static bool InitHK(FOnlineVoiceImpl* This);
    static void* SerializeRemotePacketHK(FOnlineVoiceImpl* This, void* OutPacket, void* Ar);
};
