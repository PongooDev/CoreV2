#pragma once

#include <atomic>
#include <cstdint>

struct FUniqueNetIdRepl;

// The engine's voice interface (OnlineSubsystemUtils' FOnlineVoiceImpl), on clients and servers: Core logs whether it
// started and counts the voice packets that reach it, so a voice chat test shows where the voice stops.
class FOnlineVoiceImpl
{
public:
    static void Hook();
    // Run once, on the engine's thread, as the online subsystem starts its voice interface (FOnlineVoiceImpl::Init),
    // before Init reads [OnlineSubsystem] bHasVoiceEnabled and the voice engine it makes loads the voice module, which
    // reads [Voice] bEnabled. Packed builds set it to merge those switches into the engine's config.
    static inline std::atomic<void (*)()> BeforeInit = nullptr;
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
