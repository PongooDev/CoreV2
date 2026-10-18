#include "pch.h"

static DWORD WINAPI MainThread(LPVOID)
{
    if (MH_Initialize() != MH_OK)
    {
        return 1;
    }
    return 0;
}

BOOL APIENTRY DllMain(HMODULE Module, DWORD Reason, LPVOID)
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(Module);
        if (HANDLE Thread = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr))
        {
            CloseHandle(Thread);
        }
    }
    return TRUE;
}
