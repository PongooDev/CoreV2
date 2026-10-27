#pragma once

class FFortMcpConfig
{
public:
    // The OAuth client of MCP's "DedicatedServer" service permission, which the Cert build has users of but no entry
    // for (Core's choice: Epic's id and secret aren't in the binary; Core-Backend src/accounts/clients.js).
    static constexpr const wchar_t* DedicatedServerClientId = L"313dfed3ec3023e52d3be9dda178bdfa";
    static constexpr const wchar_t* DedicatedServerClientSecret = L"111aa434a6780d54f5cf759149639f20";

    static void Hook();
};
