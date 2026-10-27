#pragma once

#include <cstdint>

struct FUniqueNetIdRepl;

// The engine's voice interface (OnlineSubsystemUtils' FOnlineVoiceImpl), on clients and servers: Core logs whether it
// started and counts the voice packets that reach it, so a voice chat test shows where the voice stops.
class FOnlineVoiceImpl
{
public:
    static void Hook();
    // Client: registers the local player as a talker, which starting the microphone needs; once is enough.
    static void RegisterLocalTalker(uint32_t LocalUserNum);

    // Client: whether a player is talking in the engine's voice chat: their voice packets reached this client a moment
    // ago, or, for the local player, the microphone recorded some.
    static bool IsRemotePlayerTalking(const FUniqueNetIdRepl& PlayerId);
    static bool IsLocalPlayerTalking();
    // Client: runs every frame on the game thread, from the voice interface's tick.
    static inline void (*OnTick)() = nullptr;

private:
    static bool InitHK(FOnlineVoiceImpl* This);
    static void* SerializeRemotePacketHK(FOnlineVoiceImpl* This, void* OutPacket, void* Ar);
    static void* GetLocalPacketHK(FOnlineVoiceImpl* This, void* OutPacket, uint32_t LocalUserNum);
    static void TickHK(FOnlineVoiceImpl* This, float DeltaTime);
};
