#include "pch.h"
#include "Online/FortHotfixManager.h"
#include "FortContentEncryptionManager.h"
#include "Containers/UnrealString.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Misc/Base64.h"
#include "Natives.h"
#include "Packed.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"

// A dedicated server never runs the client's login step that downloads the keychain (QueryKeychain ->
// FMcpKeychainHelper::Refresh -> OnNewDataEncryptionKeyAdded -> MergeInKeyChain), so its keys for the encrypted cosmetic
// paks come from its Game hotfix instead. After a hotfix, UFortHotfixManager's handler (8.00 0x12BB020) reads the game ini's
// [HotfixedEncryptionKeys] Keys ("<guid>,<base64 key>"), hashes the keychain it built and merges it, then reloads the
// NetDrivers' DDoS settings. The client exe keeps the reading and parsing but not the step that puts each key in the
// keychain (5.21, 8.00 and 8.20 all go straight from FBase64::Decode to freeing the key), so a server merged an empty
// keychain and never mounted pakchunk1000+ (PongooDev/CoreV2#547). The redirect restores that step where the game hashes.

namespace
{
    // 5.21 (CL-4288479): the first build whose cosmetics ship in keychain-encrypted paks.
    constexpr uint32_t FirstKeychainChangelist = 4288479;
    constexpr int32_t AESKeySize = 32;

    // "<guid>,<base64 key>", optionally as "(<guid>,<base64 key>)", the way the handler splits and trims it.
    bool ParseHotfixedKey(const FString& Entry, FGuid& OutGuid, TArray<uint8_t>& OutBytes)
    {
        FString GuidString;
        FString KeyString;
        if (!Entry.Split(L",", &GuidString, &KeyString))
        {
            return false;
        }
        GuidString.RemoveFromStart(L"(");
        KeyString.RemoveFromEnd(L")");
        GuidString = GuidString.TrimStartAndEnd();
        KeyString = KeyString.TrimStartAndEnd();
        return FGuid::Parse(GuidString, OutGuid) && FBase64::GetDecodedDataSize(KeyString) == AESKeySize && FBase64::Decode(KeyString, OutBytes);
    }

    // FortniteGame.FortKeyChain's reflected size tells the two layouts apart (0x18 array, 0x58 map).
    bool KeyChainHoldsArray()
    {
        static const bool bArray = []
        {
            UObject* Found = StaticFindObject(nullptr, nullptr, L"/Script/FortniteGame.FortKeyChain", false);
            const bool bStruct = Found && Found->GetClass()->HasAnyCastFlag(CASTCLASS_UScriptStruct);
            return !bStruct || static_cast<UStruct*>(Found)->GetPropertiesSize() == sizeof(FFortKeyChain);
        }();
        return bArray;
    }
}

void UFortHotfixManager::Hook()
{
#if WITH_SERVER_CODE
    if (Packed::Changelist() < FirstKeychainChangelist || !Finder::ResolveHotfixedEncryptionKeys())
    {
        return;
    }
    Hooks::RedirectCall(L"UFortHotfixManager [HotfixedEncryptionKeys] keychain", Sites::FortHotfixManagerKeyChainHashCall, reinterpret_cast<void*>(&UpdateKeyChainHashHK));
#endif
}

void UFortHotfixManager::UpdateKeyChainHashHK(void* KeyChain)
{
#if WITH_SERVER_CODE
    TArray<FString> Entries;
    Natives::FConfigCacheIni_GetArray(*Natives::GConfig, L"HotfixedEncryptionKeys", L"Keys", Entries, *Natives::GGameIni);
    int32_t Added = 0;
    for (const FString& Entry : Entries)
    {
        FGuid Guid;
        TArray<uint8_t> Bytes;
        if (!ParseHotfixedKey(Entry, Guid, Bytes))
        {
            Log::Warning(L"Hotfixed encryption keys: an entry isn't \"<guid>,<base64 key>\", skipped");
            continue;
        }
        if (KeyChainHoldsArray())
        {
            TArray<FFortEncryptionKey>& Keys = static_cast<FFortKeyChain*>(KeyChain)->Keys;
            FFortEncryptionKey& NewKey = Keys[Keys.Emplace()];
            NewKey.Guid = Guid;
            NewKey.Bytes = std::move(Bytes);
        }
        else
        {
            static_cast<FFortKeyChainByGuid*>(KeyChain)->Keys.Add(Guid, FFortEncryptionKeyBytes{ std::move(Bytes) });
        }
        ++Added;
    }
    if (Entries.Num() > 0)
    {
        Log::Info(L"Hotfixed encryption keys: {} of {} added to the keychain", Added, Entries.Num());
    }
#endif
    Natives::FFortKeyChain_UpdateHash(KeyChain);
}
