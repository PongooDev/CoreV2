#include "pch.h"
#include "FortGameInstance.h"
#include "Engine/Engine.h"
#include "Finder.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Packed.h"

void UFortGameInstance::Init()
{
    InitOG(this);

#if UE_SERVER
    TooltipManager = nullptr;
#endif
}

void UFortGameInstance::Hook()
{
    if (uint8_t* Check = Sites::FortGameInstanceStartDedicatedCheck)
    {
        const uint8_t Skip[] = { 0xEB, static_cast<uint8_t>(Sites::FortGameInstanceStartDedicatedTarget - (Check + 2)) };
        Hooks::WriteBytes(L"UFortGameInstance::StartGameInstance IsDedicatedServerInstance", Check, Skip);
    }

    // The function that names the chatroom, then the function the piece holding the name jumps back into.
    std::vector<uintptr_t> Candidates;
    // 6.10 (CL-4480234): the packer put the piece inside another function's .pdata range (lea at 0x89C097, in 0x89BE20's
    // 0x89BE20-0x89C129), and that function is a virtual three vtable slots hold, so the reference's function passed for
    // Init: Core swapped those three slots and left Init (0xE81770) alone, and the server died at the game state's
    // session id change for want of the dedicated server's analytics provider. Init is the function the piece provably
    // belongs to: the one it jumps back into (0xE81957), whose own jmp to the piece (0xE81946 to 0x89C091, the mov r8d,
    // 1 before the lea) sits right before that return address. Those functions are tried before the others.
    std::vector<uintptr_t> Owners;
    for (uintptr_t Reference : Finder::StringRefs(L"TeamChatroom"))
    {
        Candidates.push_back(Finder::FunctionStart(Reference));
        // 5.40 (CL-4352937): the packer moved the piece that names the chatroom into its own section, with no .pdata
        // entry (lea rdx, "TeamChatroom" at 0x5CD0E6A), and it jumps back into Init's body (jmp 0xE33718), whose
        // .pdata entry chains to Init's (0xE33550). The first jmp after the reference leads back. 5.41 (CL-4363240) moved
        // the piece within the first .text and gave it a .pdata entry of its own, unchained (0x8B50D0, lea at 0x8B5184),
        // so the reference's function is the piece, which no vtable holds; its jmp (0x8B518F) leads back to 0xE34AE8,
        // whose entry chains to Init's (0xE34920).
        for (uintptr_t Jump = Reference + 7; Jump + 5 <= Reference + 0x20; ++Jump)
        {
            if (*reinterpret_cast<const uint8_t*>(Jump) == 0xE9)
            {
                const uintptr_t Return = Finder::RelativeTarget(Jump, 1, 5);
                const uintptr_t Owner = Finder::FunctionStart(Return);
                Candidates.push_back(Owner);
                for (uintptr_t Leave = Owner && Return - Owner >= 0x20 ? Return - 0x20 : 0; Leave && Leave + 5 <= Return; ++Leave)
                {
                    const uintptr_t Piece = *reinterpret_cast<const uint8_t*>(Leave) == 0xE9 ? Finder::RelativeTarget(Leave, 1, 5) : 0;
                    if (Piece && Piece <= Reference && Reference - Piece <= 0x10)
                    {
                        Owners.push_back(Owner);
                        break;
                    }
                }
                break;
            }
        }
    }
    std::erase(Candidates, uintptr_t{ 0 });
    if (Candidates.empty())
    {
        Log::Error(L"Finder: UFortGameInstance::Init not found");
        return;
    }
    // UGameInstance::Init runs inside UEngine::Init, before a 3.5+ build's packer checks the code on the first ticks
    // (Packed::ChecksCodeOnFirstTicks), when Core holds its detours back. It is virtual, so those builds swap it in the
    // vtables that hold it, data the check doesn't look at.
    if (Packed::ChecksCodeOnFirstTicks())
    {
        uintptr_t Init = 0;
        std::vector<void**> Slots;
        Owners.insert(Owners.end(), Candidates.begin(), Candidates.end());
        for (const uintptr_t Candidate : Owners)
        {
            Slots = Finder::ReadOnlySlotsHolding(Candidate);
            if (!Slots.empty())
            {
                Init = Candidate;
                break;
            }
        }
        if (Slots.empty())
        {
            Log::Error(L"Finder: UFortGameInstance::Init is in no vtable");
            return;
        }
        InitOG = reinterpret_cast<decltype(InitOG)>(Init);
        for (void** Slot : Slots)
        {
            Hooks::SwapVTable(L"UFortGameInstance::Init", Slot, 0, reinterpret_cast<void*>(&InitHK));
        }
        return;
    }
    Hooks::Detour(L"UFortGameInstance::Init", reinterpret_cast<void*>(Candidates.front()), &InitHK, &InitOG);
}

void UFortGameInstance::InitHK(UFortGameInstance* This)
{
    // On 3.5+ Init runs before Core's UGameInstance::IsDedicatedServerInstance detour is in, and makes the server's
    // analytics provider (which the game mode's session id change reports to) only for a dedicated server instance, so
    // the instance is marked as one the way IsDedicatedServerInstance reads it, in its world context.
    if (Packed::ChecksCodeOnFirstTicks() && This->WorldContext)
    {
        This->WorldContext->RunAsDedicated = true;
    }
    This->Init();
}
