#pragma once

class UFortHotfixManager
{
public:
    static void Hook();

private:
    // The handler's keychain hash call, redirected: adds the [HotfixedEncryptionKeys] keys, then hashes.
    static void UpdateKeyChainHashHK(void* KeyChain);
};
