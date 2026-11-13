#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
#include "Finder.h"
#include "Hooks.h"
#include "HttpRedirect.h"
#include "Log.h"
#include "Natives.h"
#include "Packed.h"
#include "UnsafeEnvironment.h"
#include "XmppClient.h"

static void Start();

// The runtime addresses the redirect itself uses: GMalloc, for the URLs it hands the engine (FString), and on a packed
// build GEngine, and GWorld where the packer checks its code again on the first ticks (Packed::WaitForCodeCheck). The
// others are for the client's and the gameserver's code, which this DLL doesn't run.
static bool HasRedirectAddresses()
{
    if (!Natives::GMalloc)
    {
        return false;
    }
    return !Packed::IsPacked() || (Natives::GEngine && (!Packed::ChecksCodeOnFirstTicks() || Natives::GWorld));
}

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Redirect");
    // A crashed server's crash reporter could hold this log too, and the launcher moves it before the server's (CoreV2 #352).
    Log::KeepFromChildProcesses();
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

    // One of the client's or the gameserver's addresses missing mustn't leave the client on Epic's login screen: on 6.10
    // CL-4464155 StaticConstructObject_Internal did (Core-Backend #340), though the redirect never constructs an object.
    if (!Finder::ResolveRuntime())
    {
        if (!HasRedirectAddresses())
        {
            Log::Error(L"Startup aborted: required addresses are missing");
            return;
        }
        Log::Warning(L"Finder: the redirect doesn't use the addresses missing above, so it starts without them");
    }

    if (!HttpRedirect::Hook())
    {
        Log::Error(L"Startup aborted: the HTTP requests could not be redirected");
        return;
    }

    buzz::XmppClient::Private::Hook();
    // Packed builds only: the packer checks its decrypted code as the initial load ends, and patches
    // present at that moment hang the game, so the detours go in once the engine is up.
    Packed::WaitForCodeCheck(true);
    Hooks::EnableAll();
    UnsafeEnvironment::Null();

    Log::Info(L"Core Redirect startup complete");
}
