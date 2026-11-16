#pragma once

#include "Online/VoiceChatTokenFeature.h"

#include <cstdint>
#include <string>

// The server's "VoiceChatToken" feature for the game's own Vivox voice chat (the client plugin "Vivox", FVivox): Vivox
// access tokens for AFortGameModeZone's voice login and channel joins, which the game mode sends its players with
// ClientVoiceChatLogin and ClientVoiceChatChannelJoin. Epic's server-only plugin that registered it isn't in any client
// build nor in the 21.40 PDB, so the module and class names are Core's; the interface is the game's.
//
// The tokens are Vivox's format, signed with the key Core-Backend's Vivox emulator shares with its dedicated servers
// (Core-Backend services/core/vivox.js; the contract is H:\CoreWork\vivox\contract.md):
//   e30.<base64url(payload)>.<base64url(HMAC-SHA256(key, "e30.<base64url(payload)>"))>
//   payload {"iss":<issuer>,"exp":<unix seconds>,"vxa":"login"|"join","vxi":<unique>,"f":<from>[,"t":<to>]}
// with "e30" the base64url of "{}" (an empty header), base64url without padding and the key's UTF-8 bytes as given.
// f is "sip:" + the player's voice name (".<accountId>.") + "@" + domain; t is the channel's SIP URI by the client's
// own rule (FVivox, 7.00 0x999620): a name that starts with "sip:" as it is, else "sip:" + name + "@" + domain.
// The game calls the two on its game thread and waits for the answer, which is why the server signs them itself
// instead of asking the backend for each.
class FVivoxVoiceChatToken final : public IVoiceChatTokenFeature
{
public:
    // Issuer, Domain and Key as the backend's signing answer gives them; TokenLifetimeSeconds is each token's life.
    FVivoxVoiceChatToken(std::string InIssuer, std::string InDomain, std::string InKey, int32_t InTokenLifetimeSeconds);

    virtual void GenerateClientLoginToken(const FString& PlayerName, FString& OutToken) override;
    virtual void GenerateClientJoinToken(const FString& PlayerName, const FString& ChannelName, FString& OutToken) override;

private:
    // A signed token for Action ("login" or "join"), from From to To (no "t" when To is empty); "" if signing fails.
    std::string GenerateToken(const char* Action, const std::string& From, const std::string& To, std::wstring& OutDescription);

    std::string Issuer;
    std::string Domain;
    std::string Key;
    int32_t TokenLifetimeSeconds = 300;
    // vxi, the token's serial: the time in microseconds, or one past the last one given.
    int64_t LastSerial = 0;
};

// The module: at server start, where the build's game mode asks for the feature and its clients can reach Core's Vivox
// emulator, it asks the backend for its Vivox signing (with the DedicatedServer client) and registers the feature with
// the engine (IModularFeatures::RegisterModularFeature) when the backend's Vivox switch is on.
class FVivoxVoiceChatTokenModule
{
public:
    static void StartupModule();
};
