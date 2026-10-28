#include "pch.h"
#include "XmppConnectionJingle.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"

#include <algorithm>

namespace
{
    int32_t IsQuittingSlot = -1;

    // How long the connection's thread waits while the XMPP thread is stopped: what its message pump waits
    // (ProcessMessages(100)) while it runs.
    constexpr DWORD StoppedWaitMs = 100;

    // FXmppConnectionJingle::Run's check of its XMPP thread (1.8 0x2CCB7CF, 1.7.2 0x2C4EDFF, 1.2 0x2A985DF):
    // "mov rcx, [this+XmppThread]; mov rax, [rcx]; call [rax+IsQuitting]; test al, al; jne <loop end>", and the loop end
    // follows the pump's "mov edx, 100; call ProcessMessages". Returns the "mov rax, [rcx]".
    uintptr_t FindQuittingCheck(uintptr_t Begin, uintptr_t End)
    {
        for (uintptr_t Address = Begin; Address + 0x10 <= End; ++Address)
        {
            const auto* Code = reinterpret_cast<const uint8_t*>(Address);
            const bool bCheck = Code[0] == 0x48 && Code[1] == 0x8B && (Code[2] & 0xF8) == 0x48 && Code[2] != 0x4C && Code[4] == 0x48 && Code[5] == 0x8B && Code[6] == 0x01
                && Code[7] == 0xFF && Code[8] == 0x50 && Code[10] == 0x84 && Code[11] == 0xC0 && Code[12] == 0x75;
            const auto* LoopEnd = bCheck ? Code + 14 + static_cast<int8_t>(Code[13]) : nullptr;
            if (LoopEnd && LoopEnd[-10] == 0xBA && *reinterpret_cast<const int32_t*>(LoopEnd - 9) == static_cast<int32_t>(StoppedWaitMs) && LoopEnd[-5] == 0xE8)
            {
                IsQuittingSlot = Code[9] / 8;
                return Address + 4;
            }
        }
        return 0;
    }
}

bool FXmppConnectionJingle::IsXmppThreadQuittingHK(void* XmppThread)
{
    using FIsQuitting = bool (*)(void*);
    const bool bQuitting = reinterpret_cast<FIsQuitting>((*reinterpret_cast<void***>(XmppThread))[IsQuittingSlot])(XmppThread);
    if (bQuitting)
    {
        Sleep(StoppedWaitMs);
    }
    return bQuitting;
}

bool FXmppConnectionJingle::Hook()
{
    // The connection's thread pumps the XMPP thread's messages, waiting up to 100ms for each, only while that thread
    // runs: once the connection fails or logs out, the XMPP thread is stopped and the loop goes round with no wait until
    // the next login, a whole core for as long as the process lives. A server logs in at boot, before the backend's
    // hotfix points XMPP at it, so its connection goes to Epic's XMPP host, fails 20s later, and every Core server sat
    // at a full core. While the XMPP thread is stopped the loop now waits as the pump would.
    for (const uintptr_t Ref : Finder::StringRefs(L"OnSocketClosed state=%d"))
    {
        // Run is in the same file as the socket handler it binds (1.8: 0x2CCB3F0 and 0x2CC47D0).
        const uintptr_t Near = Finder::FunctionStart(Ref);
        const uintptr_t Check = Near ? FindQuittingCheck(std::max(Near, Finder::TextBegin() + 0x40000) - 0x40000, std::min(Near + 0x40000, Finder::TextEnd())) : 0;
        if (!Check)
        {
            continue;
        }

        const uint8_t Call[] = { 0xE8, 0x00, 0x00, 0x00, 0x00, 0x90 };
        return Hooks::WriteBytes(L"FXmppConnectionJingle::Run IsQuitting", reinterpret_cast<void*>(Check), Call)
            && Hooks::RedirectCall(L"FXmppConnectionJingle::Run IsQuitting", reinterpret_cast<uint8_t*>(Check), reinterpret_cast<void*>(&IsXmppThreadQuittingHK));
    }

    Log::Warning(L"Redirect: FXmppConnectionJingle::Run's XMPP thread check not found, a failed XMPP connection keeps a core busy");
    return false;
}
