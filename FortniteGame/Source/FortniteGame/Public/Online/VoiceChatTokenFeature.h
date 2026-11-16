#pragma once

#include "Containers/UnrealString.h"
#include "Features/IModularFeature.h"
#include "UObject/NameTypes.h"

// What a server's AFortGameModeZone (and its subclasses) asks for its players' Vivox access tokens: the modular
// feature "VoiceChatToken", which a server-only plugin registers (none of the client builds Core's servers run has one,
// so without Core the game mode finds no implementation and sends no voice login at all). Name and functions from the
// 21.40 PDB (IVoiceChatTokenFeature : IModularFeature, GenerateClientLoginToken and GenerateClientJoinToken, there
// asynchronous, with a delegate for the answer); 1.7.2 to 7.10 call the two synchronously and send the token right
// after (7.00):
// - AFortGameModeZone's voice login (slot +0xBE8, 0xEE0690; AFortGameModeAthena's 0xC23370 for a team of more than
//   one): GenerateClientLoginToken(PlayerController's voice name ".<accountId>.", Token), then
//   ClientVoiceChatLogin(name, Token) (0x1627BA0).
// - Its channel join (+0xBF0, 0xEE0480; Athena's 0xC23250, same team test): the channel from the game mode's
//   GenerateVoiceChatChannel (+0xC00), GenerateClientJoinToken(name, channel, Token), then
//   ClientVoiceChatChannelJoin(name, channel, Token) (0x1627A10). Athena's +0xC08 (0xC23490, after a team change) asks
//   for the 3D channel's join the same way, unless bDisable3DVoiceChat.
// The game reads both slots straight off the feature's vtable (slot 0, slot 1) and frees Token's buffer itself.
class IVoiceChatTokenFeature : public IModularFeature
{
public:
    // The name the game looks the feature up by (FName("VoiceChatToken"), 7.00 0xEE0890).
    static FName GetModularFeatureName()
    {
        static const FName FeatureName(L"VoiceChatToken");
        return FeatureName;
    }

    // A login token for PlayerName, the player's Vivox account name.
    virtual void GenerateClientLoginToken(const FString& PlayerName, FString& OutToken) = 0;
    // A token for PlayerName to join ChannelName.
    virtual void GenerateClientJoinToken(const FString& PlayerName, const FString& ChannelName, FString& OutToken) = 0;
};
