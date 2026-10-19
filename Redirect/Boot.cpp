#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
#include "Finder.h"
#include "HttpRedirect.h"
#include "Log.h"

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(L"Redirect");
    Log::Info(L"Core Redirect starting");

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

    Log::Info(L"Core Redirect startup complete");
}
