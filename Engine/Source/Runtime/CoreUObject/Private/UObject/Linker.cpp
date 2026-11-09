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
        if (Sites::LinkerFilterRegisterStore)
        {
            // 5.0: mov [base+disp32], r12b (REX 88 ModRm disp32) becomes mov byte ptr [base+disp32], 0 (C6 ModRm disp32 00).
            const uint8_t Store[7] = { 0xC6, static_cast<uint8_t>(Filter[2] & 0xC7), Filter[3], Filter[4], Filter[5], Filter[6], 0x00 };
            memcpy(ClientCookedContent, Store, sizeof(ClientCookedContent));
        }
        Hooks::WriteBytes(L"FLinker::FLinker FilterClientButNotServer", Filter, ClientCookedContent);
    }
}
