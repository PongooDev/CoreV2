#include "pch.h"
#include "Athena/AthenaBarrierObjective.h"
#include "Finder.h"
#include "Log.h"
#include "UObject/Class.h"

namespace
{
    // The offset OnGeneratorDestroyed broadcasts the delegate at: its exec ends in a jump to the native (6.30 0x14940E0
    // -> 0xC50470), whose first "lea r64, [rcx+disp32]" is the delegate ("lea rsi, [rcx+740h]"). -1 where not found.
    ptrdiff_t FindObjectiveDestroyedOffset()
    {
        UFunction* Function = AAthenaBarrierObjective::FindStaticFunction_OnGeneratorDestroyed();
        const uint8_t* Exec = Function ? reinterpret_cast<const uint8_t*>(Function->GetNativeFunc()) : nullptr;
        const uint8_t* Native = nullptr;
        for (int32_t Index = 0; Exec && Index < 0x40 && !Native; ++Index)
        {
            if (Exec[Index] == 0xE9)
            {
                Native = Exec + Index + 5 + *reinterpret_cast<const int32_t*>(Exec + Index + 1);
            }
        }
        for (int32_t Index = 0; Native && Index < 0x40; ++Index)
        {
            // REX.W (48, or 4C for r8 to r15) 8D, ModRM mod 10 with rm 001 (rcx), disp32.
            if ((Native[Index] == 0x48 || Native[Index] == 0x4C) && Native[Index + 1] == 0x8D && (Native[Index + 2] & 0xC7) == 0x81)
            {
                const int32_t Offset = *reinterpret_cast<const int32_t*>(Native + Index + 3);
                return Offset > 0 && Offset < 0x2000 ? Offset : -1;
            }
        }
        return -1;
    }
}

TMulticastDelegate<AAthenaBarrierObjective*>* AAthenaBarrierObjective::GetOnObjectiveDestroyed() const
{
    static const ptrdiff_t Offset = FindObjectiveDestroyedOffset();
    static bool bLogged = false;
    if (!bLogged)
    {
        bLogged = true;
        if (Offset > 0)
        {
            Log::Info(L"Finder: AAthenaBarrierObjective::OnGeneratorDestroyed broadcasts the objective-destroyed delegate at +0x{:X}", Offset);
        }
        else
        {
            Log::Error(L"Finder: AAthenaBarrierObjective::OnGeneratorDestroyed's delegate was not found, a destroyed mascot ends no Food Fight match");
        }
    }
    return Offset > 0 ? reinterpret_cast<TMulticastDelegate<AAthenaBarrierObjective*>*>(reinterpret_cast<uint8_t*>(const_cast<AAthenaBarrierObjective*>(this)) + Offset) : nullptr;
}

void AAthenaBarrierObjective::SetAllowDamage(bool bInAllowDamage)
{
#if WITH_SERVER_CODE
    if (bAllowDamage != bInAllowDamage)
    {
        bAllowDamage = bInAllowDamage;
        ForceNetUpdate();
    }
#endif
}
