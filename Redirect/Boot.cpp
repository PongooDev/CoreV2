#include "pch.h"
#include "Boot.h"
#include "Log.h"

void Boot::Initialize()
{
    Log::Initialize(L"Redirect", false);
    Log::Info(L"Core Redirect starting");

    if (MH_Initialize() != MH_OK)
    {
        Log::Error(L"MinHook failed to initialize");
        return;
    }

    Log::Info(L"Core Redirect startup complete");
}
