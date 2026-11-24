#include "pch.h"
#include "VivoxVoiceChatToken.h"
#include "BackendRequest.h"
#include "Features/IModularFeatures.h"
#include "Finder.h"
#include "GameThread.h"
#include "Log.h"
#include "Online/NotForLicensees/FortMcpConfig.h"

#include <bcrypt.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string_view>
#include <type_traits>
#include <utility>

#pragma comment(lib, "bcrypt.lib")

namespace
{
    std::string ToUtf8(std::wstring_view Text)
    {
        const int32_t Size = Text.empty() ? 0 : WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int32_t>(Text.size()), nullptr, 0, nullptr, nullptr);
        std::string Result(static_cast<size_t>((std::max)(Size, 0)), '\0');
        if (Size > 0)
        {
            WideCharToMultiByte(CP_UTF8, 0, Text.data(), static_cast<int32_t>(Text.size()), Result.data(), Size, nullptr, nullptr);
        }
        return Result;
    }

    std::wstring ToWide(std::string_view Text)
    {
        const int32_t Size = Text.empty() ? 0 : MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int32_t>(Text.size()), nullptr, 0);
        std::wstring Result(static_cast<size_t>((std::max)(Size, 0)), L'\0');
        if (Size > 0)
        {
            MultiByteToWideChar(CP_UTF8, 0, Text.data(), static_cast<int32_t>(Text.size()), Result.data(), Size);
        }
        return Result;
    }

    // Base64url (RFC 4648 section 5) without padding, as Vivox's tokens have it.
    std::string Base64Url(const uint8_t* Bytes, size_t Size)
    {
        static constexpr char Alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
        std::string Result;
        Result.reserve((Size + 2) / 3 * 4);
        size_t At = 0;
        for (; At + 3 <= Size; At += 3)
        {
            const uint32_t Group = (static_cast<uint32_t>(Bytes[At]) << 16) | (static_cast<uint32_t>(Bytes[At + 1]) << 8) | Bytes[At + 2];
            Result += Alphabet[(Group >> 18) & 63];
            Result += Alphabet[(Group >> 12) & 63];
            Result += Alphabet[(Group >> 6) & 63];
            Result += Alphabet[Group & 63];
        }
        if (Size - At == 1)
        {
            const uint32_t Group = static_cast<uint32_t>(Bytes[At]) << 16;
            Result += Alphabet[(Group >> 18) & 63];
            Result += Alphabet[(Group >> 12) & 63];
        }
        else if (Size - At == 2)
        {
            const uint32_t Group = (static_cast<uint32_t>(Bytes[At]) << 16) | (static_cast<uint32_t>(Bytes[At + 1]) << 8);
            Result += Alphabet[(Group >> 18) & 63];
            Result += Alphabet[(Group >> 12) & 63];
            Result += Alphabet[(Group >> 6) & 63];
        }
        return Result;
    }

    std::string Base64Url(std::string_view Text)
    {
        return Base64Url(reinterpret_cast<const uint8_t*>(Text.data()), Text.size());
    }

    // A JSON string literal of UTF-8 Text.
    std::string JsonString(std::string_view Text)
    {
        std::string Result = "\"";
        for (const char Char : Text)
        {
            const uint8_t Code = static_cast<uint8_t>(Char);
            if (Char == '"' || Char == '\\')
            {
                Result += '\\';
                Result += Char;
            }
            else if (Code < 0x20)
            {
                Result += std::format("\\u{:04x}", Code);
            }
            else
            {
                Result += Char;
            }
        }
        return Result + "\"";
    }

    // HMAC-SHA256 of Text with Key's bytes, through Windows' CNG.
    bool HmacSha256(const std::string& Key, const std::string& Text, uint8_t (&OutDigest)[32])
    {
        BCRYPT_ALG_HANDLE Algorithm = nullptr;
        if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG)))
        {
            return false;
        }
        BCRYPT_HASH_HANDLE Hash = nullptr;
        const bool bHashed = BCRYPT_SUCCESS(BCryptCreateHash(Algorithm, &Hash, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(Key.data())), static_cast<ULONG>(Key.size()), 0))
            && BCRYPT_SUCCESS(BCryptHashData(Hash, reinterpret_cast<PUCHAR>(const_cast<char*>(Text.data())), static_cast<ULONG>(Text.size()), 0))
            && BCRYPT_SUCCESS(BCryptFinishHash(Hash, OutDigest, sizeof(OutDigest), 0));
        if (Hash)
        {
            BCryptDestroyHash(Hash);
        }
        BCryptCloseAlgorithmProvider(Algorithm, 0);
        return bHashed;
    }

    // The feature the engine holds. Epic's module would own it and unregister it in ShutdownModule; Core's server ends
    // with its process, so it stays registered (and allocated) until then.
    IModularFeature* RegisteredFeature = nullptr;

    // FeatureType is FVivoxVoiceChatToken (5.10 to 8.00's FVivox) or FVivoxVoiceChatChannelToken (8.20 to 8.40's UE 4.23
    // VivoxVoiceChat), whichever interface the build's game mode calls.
    template <typename FeatureType>
    void RegisterFeature(std::string Issuer, std::string Domain, std::string Key, int32_t TokenLifetimeSeconds)
    {
        if (RegisteredFeature)
        {
            return;
        }
        const std::wstring IssuerText = ToWide(Issuer);
        const std::wstring DomainText = ToWide(Domain);
        FeatureType* Feature = new FeatureType(std::move(Issuer), std::move(Domain), std::move(Key), TokenLifetimeSeconds);
        // The game takes the IModularFeature* the registry gives back 8 bytes down to the interface (7.00 0xEE0731, "lea
        // rsi, [rax-8]"; 8.30's 0x12354D0 the same): the interface's vtable pointer, then its empty IModularFeature base.
        const ptrdiff_t BaseOffset = reinterpret_cast<uintptr_t>(static_cast<IModularFeature*>(Feature)) - reinterpret_cast<uintptr_t>(Feature);
        if (BaseOffset != 8)
        {
            Log::Error(L"VoiceChatToken: Core's feature has its IModularFeature at +{}, not +8 as the game reads it, so it isn't registered", BaseOffset);
            delete Feature;
            return;
        }
        RegisteredFeature = Feature;

        const FName FeatureName = IVoiceChatTokenFeature::GetModularFeatureName();
        IModularFeatures& ModularFeatures = IModularFeatures::Get();
        ModularFeatures.RegisterModularFeature(FeatureName, Feature);

        // The lookup the game mode makes before each token (IsModularFeatureAvailable, then GetModularFeature) must give
        // this feature back.
        const int32_t Count = ModularFeatures.GetModularFeatureImplementationCount(FeatureName);
        const FeatureType* Found = Count > 0 ? &ModularFeatures.GetModularFeature<FeatureType>(FeatureName) : nullptr;
        if (Found != Feature)
        {
            Log::Error(L"VoiceChatToken: registered, but the engine lists {} implementation(s) and its first isn't Core's, so the game mode won't use it", Count);
            return;
        }
        Log::Info(L"VoiceChatToken: registered Core's Vivox token feature for {} (issuer {}, domain {}, tokens live {} s); the engine lists {} implementation(s), Core's first",
            std::is_same_v<FeatureType, FVivoxVoiceChatChannelToken> ? L"UE 4.23's VivoxVoiceChat" : L"FVivox", IssuerText, DomainText, TokenLifetimeSeconds, Count);
    }
}

FVivoxVoiceChatToken::FVivoxVoiceChatToken(std::string InIssuer, std::string InDomain, std::string InKey, int32_t InTokenLifetimeSeconds)
    : Issuer(std::move(InIssuer))
    , Domain(std::move(InDomain))
    , Key(std::move(InKey))
    , TokenLifetimeSeconds(InTokenLifetimeSeconds > 0 ? InTokenLifetimeSeconds : 300)
{
}

void FVivoxVoiceChatToken::GenerateClientLoginToken(const FString& PlayerName, FString& OutToken)
{
    std::wstring Description;
    const std::string Token = GenerateToken("login", "sip:" + ToUtf8(PlayerName.ToStdString()) + "@" + Domain, std::string(), Description);
    OutToken = FString(ToWide(Token));
    Log::Info(L"VoiceChatToken: the game mode asked for {}'s login token: {}", PlayerName.ToStdString(), Description);
}

void FVivoxVoiceChatToken::GenerateClientJoinToken(const FString& PlayerName, const FString& ChannelName, FString& OutToken)
{
    // The URI the client will join (FVivox's channel URI, 7.00 0x999620): a name that starts with "sip:", in any case,
    // as it is, else sip:<name>@<domain>.
    const std::string Channel = ToUtf8(ChannelName.ToStdString());
    const std::string To = Channel.size() >= 4 && _strnicmp(Channel.c_str(), "sip:", 4) == 0 ? Channel : "sip:" + Channel + "@" + Domain;
    std::wstring Description;
    const std::string Token = GenerateToken("join", "sip:" + ToUtf8(PlayerName.ToStdString()) + "@" + Domain, To, Description);
    OutToken = FString(ToWide(Token));
    Log::Info(L"VoiceChatToken: the game mode asked for {}'s join token for {}: {}", PlayerName.ToStdString(), ChannelName.ToStdString(), Description);
}

std::string FVivoxVoiceChatToken::GenerateToken(const char* Action, const std::string& From, const std::string& To, std::wstring& OutDescription)
{
    // As the backend's own (Core-Backend vivox/tokens.js): exp now plus the lifetime, vxi the time in microseconds or
    // one past the last.
    const int64_t Now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    LastSerial = (std::max)(Now * 1000, LastSerial + 1);
    std::string Payload = "{\"iss\":" + JsonString(Issuer) + ",\"exp\":" + std::to_string(Now / 1000 + TokenLifetimeSeconds) + ",\"vxa\":" + JsonString(Action) +
        ",\"vxi\":" + std::to_string(LastSerial) + ",\"f\":" + JsonString(From);
    if (!To.empty())
    {
        Payload += ",\"t\":" + JsonString(To);
    }
    Payload += "}";

    const std::string Signed = "e30." + Base64Url(Payload);
    uint8_t Digest[32] = {};
    if (!HmacSha256(Key, Signed, Digest))
    {
        OutDescription = ToWide(Payload) + L" not signed (Windows' HMAC-SHA256 failed), so no token";
        return std::string();
    }
    const std::string Signature = Base64Url(Digest, sizeof(Digest));
    // The payload and the signature's start, as the backend logs a token: enough to check it with the key, not to use it.
    OutDescription = ToWide(Payload) + L" signature " + ToWide(Signature.substr(0, 6)) + L"...";
    return Signed + "." + Signature;
}

FVivoxVoiceChatChannelToken::FVivoxVoiceChatChannelToken(std::string InIssuer, std::string InDomain, std::string InKey, int32_t InTokenLifetimeSeconds)
    : Signer(std::move(InIssuer), std::move(InDomain), std::move(InKey), InTokenLifetimeSeconds)
{
}

std::string FVivoxVoiceChatChannelToken::AccountUri(const FString& PlayerName) const
{
    return "sip:." + Signer.GetIssuer() + "." + ToUtf8(PlayerName.ToStdString()) + ".@" + Signer.GetDomain();
}

FString FVivoxVoiceChatChannelToken::GenerateClientLoginToken(const FString& PlayerName)
{
    std::wstring Description;
    const std::string Token = Signer.GenerateToken("login", AccountUri(PlayerName), std::string(), Description);
    Log::Info(L"VoiceChatToken: the game mode asked for {}'s login token: {}", PlayerName.ToStdString(), Description);
    return FString(ToWide(Token));
}

FString FVivoxVoiceChatChannelToken::GenerateClientJoinToken(const FString& PlayerName, const FString& ChannelName, EVoiceChatChannelType ChannelType, const TOptional<FVoiceChatChannel3dProperties>& Channel3dProperties)
{
    // FVivoxVoiceChat's channel URI: g non-positional, d positional, e echo; 3D properties as Vivox's
    // "!p-<audible distance>-<conversational distance>-<rolloff>-<model>" (8.30 0x3F0D7B0).
    const char* TypeString = ChannelType == EVoiceChatChannelType::Positional ? "d" : ChannelType == EVoiceChatChannelType::Echo ? "e" : "g";
    std::string Properties;
    if (Channel3dProperties.IsSet())
    {
        const FVoiceChatChannel3dProperties& Value = Channel3dProperties.GetValue();
        Properties = std::format("!p-{}-{}-{:.3f}-{}", Value.MaxDistance, Value.MinDistance, Value.Rolloff, static_cast<int32_t>(Value.AttenuationModel));
    }
    const std::string To = "sip:confctl-" + std::string(TypeString) + "-" + Signer.GetIssuer() + "." + ToUtf8(ChannelName.ToStdString()) + Properties + "@" + Signer.GetDomain();
    std::wstring Description;
    const std::string Token = Signer.GenerateToken("join", AccountUri(PlayerName), To, Description);
    Log::Info(L"VoiceChatToken: the game mode asked for {}'s join token for {}: {}", PlayerName.ToStdString(), ChannelName.ToStdString(), Description);
    return FString(ToWide(Token));
}

void FVivoxVoiceChatTokenModule::StartupModule()
{
#if WITH_SERVER_CODE
    static bool bStarted = false;
    if (bStarted)
    {
        return;
    }
    bStarted = true;

    // Only where the build's clients can reach Core's Vivox: 5.10 (CL-4225813) is the first build whose FVivox::Connect
    // reads [Vivox] ServerUrl and Domain from the game ini (7.00 0x999130; this line is its complaint without them). 1.7.2
    // to 5.0 have Epic's server built in, so the backend points only 5.10 and later at its emulator.
    // 8.20 (CL-5625478) to 8.40 have UE 4.23's VivoxVoiceChat instead, which reads [VoiceChat.Vivox] from the engine ini
    // (8.30 0xBCDCE0, this line its complaint) and whose game mode calls the feature with channel types; 8.00 is FVivox.
    static constexpr wchar_t VivoxConfigLine[] = L"[Vivox] ServerUrl or Domain is not set";
    static constexpr wchar_t VivoxVoiceChatConfigLine[] = L"[VoiceChat.Vivox] ServerUrl, Domain, or Issuer is not set";
    const bool bFVivox = Finder::FindReadOnly(VivoxConfigLine, sizeof(VivoxConfigLine) - sizeof(wchar_t));
    const bool bVivoxVoiceChat = !bFVivox && Finder::FindReadOnly(VivoxVoiceChatConfigLine, sizeof(VivoxVoiceChatConfigLine) - sizeof(wchar_t));
    if (!bFVivox && !bVivoxVoiceChat)
    {
        Log::Info(L"VoiceChatToken: this build's Vivox, if it has one, has Epic's server built in (before 5.10), which Core's Vivox can't stand in for, so no feature is registered");
        return;
    }
    // And where its game mode asks for the feature (1.7.2 on), which is where its lookups give the registry.
    if (!Finder::ResolveModularFeaturesLive())
    {
        return;
    }

    // The key stays with the backend until a dedicated server asks for it, and only in this process's memory after.
    BackendRequest::GetJson(L"Vivox signing", L"/core/api/voice/vivox/signing", FFortMcpConfig::DedicatedServerClientId, FFortMcpConfig::DedicatedServerClientSecret,
        [bVivoxVoiceChat](uint32_t Status, const std::string& Body)
    {
        std::string Issuer = BackendRequest::JsonField(Body, "issuer");
        std::string Domain = BackendRequest::JsonField(Body, "domain");
        std::string Key = BackendRequest::JsonField(Body, "key");
        const std::string Algorithm = BackendRequest::JsonField(Body, "algorithm");
        const int32_t TokenLifetimeSeconds = std::atoi(BackendRequest::JsonField(Body, "tokenLifetimeSeconds").c_str());
        const bool bEnabled = Status == 200 && BackendRequest::JsonField(Body, "enabled") == "true";
        if (!bEnabled || Issuer.empty() || Domain.empty() || Key.empty() || (!Algorithm.empty() && Algorithm != "HMAC-SHA256"))
        {
            Log::Info(L"VoiceChatToken: not registered, the backend's Vivox signing is {} (HTTP {}); the game mode sends no voice login, as before",
                Status == 0 ? L"unanswered" : Status == 200 && !bEnabled ? L"switched off" : Status != 200 ? L"unavailable" : L"incomplete", Status);
            return;
        }
        GameThread::Enqueue([bVivoxVoiceChat, Issuer = std::move(Issuer), Domain = std::move(Domain), Key = std::move(Key), TokenLifetimeSeconds]() mutable
        {
            if (bVivoxVoiceChat)
            {
                RegisterFeature<FVivoxVoiceChatChannelToken>(std::move(Issuer), std::move(Domain), std::move(Key), TokenLifetimeSeconds);
            }
            else
            {
                RegisterFeature<FVivoxVoiceChatToken>(std::move(Issuer), std::move(Domain), std::move(Key), TokenLifetimeSeconds);
            }
        });
    });
#endif
}
