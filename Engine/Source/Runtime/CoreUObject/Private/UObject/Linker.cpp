#include "pch.h"
#include "UObject/Linker.h"
#include "Hooks.h"
#include "Natives.h"

void FLinker::Hook()
{
    if (uint8_t* Filter = Sites::LinkerFilterClientButNotServer)
    {
        uint8_t ClientCookedContent[7];
        memcpy(ClientCookedContent, Filter, sizeof(ClientCookedContent));
        ClientCookedContent[6] = 0x00;
        Hooks::WriteBytes(L"FLinker::FLinker FilterClientButNotServer", Filter, ClientCookedContent);
    }
}
