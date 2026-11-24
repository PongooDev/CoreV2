#pragma once

#include "Containers/UnrealString.h"
#include "Features/IModularFeature.h"
#include "Misc/Optional.h"
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

// UE 4.23's VoiceChat plugin types (VoiceChat.h), as 8.20 to 8.40's FVivoxVoiceChat reads them: [VoiceChat.Vivox]
// MinDistance and MaxDistance are ints there (GetInt, 8.30 0xBCDCE0), Rolloff a float.
enum class EVoiceChatChannelType : uint8_t
{
    NonPositional,
    Positional,
    Echo
};

enum class EVoiceChatAttenuationModel : uint8_t
{
    None,
    InverseByDistance,
    LinearByDistance,
    ExponentialByDistance
};

struct FVoiceChatChannel3dProperties
{
    EVoiceChatAttenuationModel AttenuationModel = EVoiceChatAttenuationModel::InverseByDistance;
    int32_t MinDistance = 100;
    int32_t MaxDistance = 3000;
    float Rolloff = 1.0f;
};

// The same feature as 8.20 to 8.40 (UE 4.23, the VoiceChat/VivoxVoiceChat plugins) lay it out: the client asks the
// backend for its own login token (POST .../voice/{accountId}/createLoginToken), so the server only signs join tokens.
// AFortGameModeZone's answer to ServerVoiceChatRequestJoinToken (8.30 0x12354D0, 8.20 0x117BE00, the same 0x422-byte
// body on 8.40) calls slot 1 as GenerateClientJoinToken(account id, channel, NonPositional, unset 3D properties) and
// sends the FString it returns with ClientVoiceChatSendJoinToken. 21.40 keeps the slot order with delegates added.
class IVoiceChatTokenFeatureWithChannelType : public IModularFeature
{
public:
    static FName GetModularFeatureName()
    {
        return IVoiceChatTokenFeature::GetModularFeatureName();
    }

    virtual FString GenerateClientLoginToken(const FString& PlayerName) = 0;
    virtual FString GenerateClientJoinToken(const FString& PlayerName, const FString& ChannelName, EVoiceChatChannelType ChannelType, const TOptional<FVoiceChatChannel3dProperties>& Channel3dProperties) = 0;
};
