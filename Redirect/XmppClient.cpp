#include "pch.h"
#include "XmppClient.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"

namespace
{
    ptrdiff_t AllowPlainOffset = -1;

    uintptr_t FindAllowPlainTest(uintptr_t Function)
    {
        const auto* Code = reinterpret_cast<const uint8_t*>(Function);
        for (int32_t Index = 0; Index < 0x20; ++Index)
        {
            if (Code[Index] == 0x45 && Code[Index + 1] == 0x84 && Code[Index + 2] == 0xC9 && Code[Index + 3] == 0x75 && Code[Index + 5] == 0x44 && Code[Index + 6] == 0x38 && Code[Index + 7] == 0x49)
            {
                return Function + Index;
            }
        }
        return 0;
    }
}

void* buzz::XmppClient::Private::ChooseBestSaslMechanismHK(Private* This, void* Result, const void* Mechanisms, bool bEncrypted)
{
    *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(This) + AllowPlainOffset) = true;
    return ChooseBestSaslMechanismOG(This, Result, Mechanisms, bEncrypted);
}

bool buzz::XmppClient::Private::Hook()
{
    for (const uintptr_t Ref : Finder::StringRefs("PLAIN"))
    {
        const uintptr_t Function = Finder::FunctionStart(Ref);
        const uintptr_t Test = Function ? FindAllowPlainTest(Function) : 0;
        if (!Test)
        {
            continue;
        }

        AllowPlainOffset = *reinterpret_cast<const uint8_t*>(Test + 8);
        return Hooks::Detour(L"XmppClient::Private::ChooseBestSaslMechanism", reinterpret_cast<void*>(Function), &ChooseBestSaslMechanismHK, &ChooseBestSaslMechanismOG);
    }

    Log::Error(L"Redirect: XmppClient::Private::ChooseBestSaslMechanism not found");
    return false;
}
