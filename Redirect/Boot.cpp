#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
#include "Finder.h"
#include "Hooks.h"
#include "HttpRedirect.h"
#include "Log.h"
#include "XmppClient.h"
#include "XmppConnectionJingle.h"

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Redirect");
    Log::Info(L"Core Redirect starting");

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
    FXmppConnectionJingle::Hook();
    Hooks::EnableAll();

    Log::Info(L"Core Redirect startup complete");
}
