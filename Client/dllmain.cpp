#include "pch.h"
#include "Boot.h"

BOOL APIENTRY DllMain(HMODULE Module, DWORD Reason, LPVOID)
{
    if (Reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(Module);
        Boot::Initialize();
    }
    return TRUE;
}
