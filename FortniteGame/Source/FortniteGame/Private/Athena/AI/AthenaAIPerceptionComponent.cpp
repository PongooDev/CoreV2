#include "pch.h"
#include "Athena/AI/AthenaAIPerceptionComponent.h"
#include "Finder.h"
#include "Log.h"
#include "UObject/Class.h"

namespace
{
    // `cmp byte ptr [rip+disp32], r8` (38 /r, with or without REX) or `cmp byte ptr [rip+disp32], imm8` (80 3D): the
    // byte it reads, or 0.
    uintptr_t ComparedStaticByte(uintptr_t Instruction)
    {
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Instruction);
        const uint32_t Rex = Code[0] >= 0x40 && Code[0] <= 0x4F ? 1 : 0;
        if (Code[Rex] == 0x38 && (Code[Rex + 1] & 0xC7) == 0x05)
        {
            return Instruction + Rex + 6 + *reinterpret_cast<const int32_t*>(Code + Rex + 2);
        }
        if (!Rex && Code[0] == 0x80 && Code[1] == 0x3D)
        {
            return Instruction + 7 + *reinterpret_cast<const int32_t*>(Code + 2);
        }
        return 0;
    }
}

void UAthenaAIPerceptionComponent::EnableServerSenses()
{
#if WITH_SERVER_CODE
    // UAthenaAIPerceptionComponent's constructor (8.20 CL-5625478 0xC36790, 8.30 0xCDA5E0, 8.00 0xB3B240) runs
    // UFortAIPerceptionComponent's (8.20 0xC91790: UAISenseConfig_Sight, _Hearing, _Damage, _Touch, _Team, _Prediction
    // into SensesConfig), then, for an instance, empties SensesConfig[0] (sight) unless one static byte is set (8.20
    // 0x5DD0485) and SensesConfig[5] (prediction) unless the next one is (0x5DD0486). Nothing in the client builds
    // writes either byte (no store, no lea of them, no pointer to them in .data or .rdata), so their setter is compiled
    // out with the server code and they stay 0: Battle Royale's AI sees nobody and only goes for a player it hears, is
    // hit by or touches (#605, Creative's creature spawner monsters). A headless 8.20 Creative server with the two set
    // had a Deimos_Brute_Red perceive a possessed player pawn by sight and take it as its GoalActor; without them no
    // creature perceived it at 157 uu.
    UClass* Class = StaticClass();
    UObject* Default = Class ? Class->GetDefaultObject(false) : nullptr;
    const uintptr_t VTable = Default ? *reinterpret_cast<const uintptr_t*>(Default) : 0;
    if (!VTable)
    {
        return;
    }
    for (const uintptr_t VTableStore : Finder::AddressRefs(VTable))
    {
        uintptr_t Flags[2] = {};
        int32_t Found = 0;
        Finder::TraceFind(VTableStore, 24, [&](uintptr_t Instruction)
        {
            if (const uintptr_t Flag = ComparedStaticByte(Instruction))
            {
                Flags[Found++] = Flag;
            }
            return Found == 2;
        });
        if (Found != 2 || Flags[1] != Flags[0] + 1)
        {
            continue;
        }
        uint8_t* const Sight = reinterpret_cast<uint8_t*>(Flags[0]);
        uint8_t* const Prediction = reinterpret_cast<uint8_t*>(Flags[1]);
        const bool bWasOn = *Sight && *Prediction;
        *Sight = 1;
        *Prediction = 1;
        Log::Info(L"UAthenaAIPerceptionComponent: Battle Royale's AI controllers keep their sight and prediction senses (the constructor's statics +0x{:X} and +0x{:X}{})",
            Flags[0] - Finder::ImageBase(), Flags[1] - Finder::ImageBase(), bWasOn ? L", already on" : L", turned on");
        return;
    }
    Log::Info(L"Finder: UAthenaAIPerceptionComponent's sense statics not found, its AI keeps the senses this build gives it");
#endif
}
