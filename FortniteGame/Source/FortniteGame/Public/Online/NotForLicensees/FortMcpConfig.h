#pragma once

class FOnlineSubsystemMcp;

class FFortMcpConfig
{
public:
    // The OAuth client of MCP's "DedicatedServer" service permission, which the Cert build has users of but no entry
    // for (Core's choice: Epic's id and secret aren't in the binary; Core-Backend src/accounts/clients.js).
    static constexpr const wchar_t* DedicatedServerClientId = L"313dfed3ec3023e52d3be9dda178bdfa";
    static constexpr const wchar_t* DedicatedServerClientSecret = L"111aa434a6780d54f5cf759149639f20";

    static void Hook();
    // Adds the "DedicatedServer" service permission above unless the config already has it.
    static void AddDedicatedServerPermissions(struct FMcpServiceConfig& ServiceConfig);

    // Packed builds only. Their hook above comes after PreInit, where every MCP service read its config, so the
    // permission is added here instead, to each service of the subsystem as the hook would have: the identity's account
    // service needs it to sign the server's client in (RegisterPlayers, UpdateInternetSession), the game service for
    // profile and world requests. Returns how many services the subsystem has (0: it isn't an MCP subsystem).
    static int32_t AddDedicatedServerPermissionsToServices(const FOnlineSubsystemMcp* Subsystem);
    // The same for the subsystems the engine has asked whether they are dedicated (FOnlineSubsystemImpl::Subsystems).
    static void AddDedicatedServerPermissionsToLoadedServices();
};
