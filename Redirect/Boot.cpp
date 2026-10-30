#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
#include "Finder.h"
#include "Hooks.h"
#include "HttpRedirect.h"
#include "Log.h"
#include "Packed.h"
#include "XmppClient.h"

static void Start();

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Redirect");
    Log::Info(L"Core Redirect starting");

    // On a packed build the image is still ciphertext at this point and the stub would overwrite anything patched
    // into it, so hand the rest over to a watcher that runs once the stub has finished.
    if (Packed::RunAfterStub(&Start))
    {
        return;
    }

    Start();
}

static void Start()
{
    if (MH_Initialize() != MH_OK)
    {
        Log::Error(L"MinHook failed to initialize");
        return;
    }

    if (!Finder::ResolveRuntime())
    {
        Log::Error(L"Startup aborted: required addresses are missing");
        return;
    }

    if (!HttpRedirect::Hook())
    {
        Log::Error(L"Startup aborted: the HTTP requests could not be redirected");
        return;
    }

    buzz::XmppClient::Private::Hook();
    // Packed builds only: the packer checks its decrypted code as the initial load ends, and patches
    // present at that moment hang the game, so the detours go in once the engine is up.
    Packed::WaitForCodeCheck();
    Hooks::EnableAll();

    Log::Info(L"Core Redirect startup complete");
}
