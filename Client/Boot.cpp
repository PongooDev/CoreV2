#include "pch.h"
#include "Boot.h"
#include "Log.h"

void Boot::Initialize()
{
    Log::Initialize(L"Client");
    Log::Info(L"Core Client starting");

    if (MH_Initialize() != MH_OK)
    {
        Log::Error(L"MinHook failed to initialize");
        return;
    }

    Log::Info(L"Core Client startup complete");
}
