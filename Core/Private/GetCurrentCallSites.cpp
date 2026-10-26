#include "pch.h"
#include "Finder.h"
#include "Log.h"

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

extern "C"
{
#include "../../ThirdParty/MinHook/src/hde/hde64.h"
}

namespace
{
    constexpr uint32_t VolatileRegisters = (1u << 0) | (1u << 1) | (1u << 2) | (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11);
    constexpr int32_t Rcx = 1;
    constexpr int32_t Rsp = 4;
    constexpr int32_t Rbp = 5;

    struct FOperand
    {
        bool bMemory = false;
        int32_t Register = -1;
        int32_t Base = -1;
        int32_t Displacement = 0;
        bool bHasDisplacement = false;
    };

    struct FState
    {
        uint32_t Held = 0;
        uint32_t VTables = 0;
        std::set<std::pair<int32_t, int32_t>> Slots;

        std::string Key() const
        {
            std::string Result = std::to_string(Held) + ":" + std::to_string(VTables);
            for (const auto& [Base, Offset] : Slots)
            {
                Result += ":" + std::to_string(Base) + "/" + std::to_string(Offset);
            }
            return Result;
        }

        bool Empty() const { return !Held && !VTables && Slots.empty(); }
    };

    struct FUse
    {
        enum EKind { Offset, VirtualCall, Call } Kind;
        uintptr_t Value;
    };

    struct FVotes
    {
        int32_t GoalManager = 0;
        int32_t Director = 0;
        int32_t Other = 0;
    };

    struct FContext
    {
        uintptr_t Stub = 0;
        void** GoalManagerVTable = nullptr;
        void** DirectorVTable = nullptr;
        int32_t GoalManagerSize = 0;
        int32_t DirectorSize = 0;
        int32_t ActorSize = 0;
        int32_t ActorSlots = 0;
        int32_t DirectorSlots = 0;
        std::map<uintptr_t, FVotes> Footprints;
    };

    FOperand RmOperand(const hde64s& Instruction)
    {
        FOperand Operand;
        if (Instruction.modrm_mod == 3)
        {
            Operand.Register = Instruction.modrm_rm | (Instruction.rex_b << 3);
            return Operand;
        }

        Operand.bMemory = true;
        if (Instruction.modrm_rm == 4)
        {
            Operand.Base = Instruction.sib_base == 5 && Instruction.modrm_mod == 0 ? -1 : (Instruction.sib_base | (Instruction.rex_b << 3));
        }
        else if (Instruction.modrm_rm == 5 && Instruction.modrm_mod == 0)
        {
            Operand.Base = -1;
        }
        else
        {
            Operand.Base = Instruction.modrm_rm | (Instruction.rex_b << 3);
        }

        if (Instruction.flags & F_DISP8)
        {
            Operand.Displacement = static_cast<int8_t>(Instruction.disp.disp8);
            Operand.bHasDisplacement = true;
        }
        else if (Instruction.flags & F_DISP32)
        {
            Operand.Displacement = static_cast<int32_t>(Instruction.disp.disp32);
            Operand.bHasDisplacement = true;
        }
        return Operand;
    }

    int32_t RegOperand(const hde64s& Instruction)
    {
        return Instruction.modrm_reg | (Instruction.rex_r << 3);
    }

    bool IsStackBase(int32_t Base)
    {
        return Base == Rsp || Base == Rbp;
    }

    bool IsTrivial(uintptr_t Function)
    {
        const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Function);
        return Bytes[0] == 0xC3
            || (Bytes[0] == 0x33 && Bytes[1] == 0xC0 && Bytes[2] == 0xC3)
            || (Bytes[0] == 0x32 && Bytes[1] == 0xC0 && Bytes[2] == 0xC3)
            || (Bytes[0] == 0xB0 && Bytes[1] == 0x01 && Bytes[2] == 0xC3)
            || (Bytes[0] == 0xC2 && Bytes[1] == 0x00 && Bytes[2] == 0x00)
            || (Bytes[0] == 0x48 && Bytes[1] == 0x33 && Bytes[2] == 0xC0 && Bytes[3] == 0xC3);
    }

    bool IsGetCurrentOfWorld(uintptr_t Site, int32_t GetWorldIndex)
    {
        const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Site - 9);
        return GetWorldIndex >= 0
            && Bytes[0] == 0xFF && (Bytes[1] & 0xF8) == 0x90 && (Bytes[1] & 0x07) != 0x04
            && *reinterpret_cast<const int32_t*>(Bytes + 2) == GetWorldIndex * 8
            && Bytes[6] == 0x48 && Bytes[7] == 0x8B && Bytes[8] == 0xC8;
    }

    uintptr_t VTableAt(void** VTable, int32_t Index)
    {
        const uintptr_t Entry = VTable ? reinterpret_cast<uintptr_t>(VTable[Index]) : 0;
        return Entry >= Finder::TextBegin() && Entry < Finder::TextEnd() ? Entry : 0;
    }

    std::vector<FUse> Walk(uintptr_t Start, uintptr_t FunctionBegin, uintptr_t FunctionEnd, int32_t SeedRegister)
    {
        std::vector<FUse> Uses;
        FState Seed;
        Seed.Held = 1u << SeedRegister;
        std::vector<std::pair<uintptr_t, FState>> Work{ { Start, Seed } };
        std::set<std::pair<uintptr_t, std::string>> Seen;
        int32_t Steps = 0;

        while (!Work.empty() && Steps < 1500)
        {
            auto [Address, State] = Work.back();
            Work.pop_back();
            while (Address >= FunctionBegin && Address < FunctionEnd && !State.Empty() && Steps < 1500)
            {
                if (!Seen.insert({ Address, State.Key() }).second)
                {
                    break;
                }
                ++Steps;

                hde64s Instruction{};
                const uint32_t Length = hde64_disasm(reinterpret_cast<const void*>(Address), &Instruction);
                if (!Length || (Instruction.flags & F_ERROR))
                {
                    break;
                }
                const uintptr_t Next = Address + Length;
                const uint8_t Opcode = Instruction.opcode;
                const bool bTwoByte = Opcode == 0x0F;
                const bool bModRm = (Instruction.flags & F_MODRM) != 0;
                const FOperand Rm = bModRm ? RmOperand(Instruction) : FOperand{};
                const int32_t Reg = bModRm ? RegOperand(Instruction) : -1;

                const bool bCallOrJump = (!bTwoByte && (Opcode == 0xE8 || Opcode == 0xE9 || Opcode == 0xEB)) || (!bTwoByte && Opcode == 0xFF && (Instruction.modrm_reg == 2 || Instruction.modrm_reg == 4));
                if (!bCallOrJump && Rm.bMemory && Rm.Base >= 0 && !IsStackBase(Rm.Base) && (State.Held & (1u << Rm.Base)) && Rm.bHasDisplacement)
                {
                    Uses.push_back({ FUse::Offset, static_cast<uint32_t>(Rm.Displacement) });
                }

                const auto Discard = [&State](int32_t Register)
                {
                    State.Held &= ~(1u << Register);
                    State.VTables &= ~(1u << Register);
                };

                if (!bTwoByte && Opcode == 0x8B && Rm.bMemory && !Rm.bHasDisplacement && Rm.Base >= 0 && (State.Held & (1u << Rm.Base)))
                {
                    State.Held &= ~(1u << Reg);
                    State.VTables |= 1u << Reg;
                }
                else if (!bTwoByte && Opcode == 0x8B)
                {
                    const bool bFromHeld = (!Rm.bMemory && (State.Held & (1u << Rm.Register)))
                        || (Rm.bMemory && IsStackBase(Rm.Base) && State.Slots.count({ Rm.Base, Rm.Displacement }));
                    if (bFromHeld)
                    {
                        State.Held |= 1u << Reg;
                    }
                    else
                    {
                        Discard(Reg);
                    }
                }
                else if (!bTwoByte && Opcode == 0x89 && !Rm.bMemory)
                {
                    if (State.Held & (1u << Reg))
                    {
                        State.Held |= 1u << Rm.Register;
                    }
                    else
                    {
                        Discard(Rm.Register);
                    }
                }
                else if (!bTwoByte && Opcode == 0x89 && Rm.bMemory && IsStackBase(Rm.Base))
                {
                    if (State.Held & (1u << Reg))
                    {
                        State.Slots.insert({ Rm.Base, Rm.Displacement });
                    }
                    else
                    {
                        State.Slots.erase({ Rm.Base, Rm.Displacement });
                    }
                }
                else if (!bTwoByte && Opcode == 0xC7 && Rm.bMemory && IsStackBase(Rm.Base))
                {
                    State.Slots.erase({ Rm.Base, Rm.Displacement });
                }
                else if ((!bTwoByte && (Opcode == 0x8D || Opcode == 0x63 || Opcode == 0x8A)) || (bTwoByte && (Instruction.opcode2 == 0xB6 || Instruction.opcode2 == 0xB7)))
                {
                    Discard(Reg);
                }
                else if (!bTwoByte && ((Opcode >= 0xB8 && Opcode <= 0xBF)))
                {
                    Discard((Opcode - 0xB8) | (Instruction.rex_b << 3));
                }
                else if (!bTwoByte && Opcode == 0xC7 && !Rm.bMemory)
                {
                    Discard(Rm.Register);
                }
                else if (!bTwoByte && (Opcode == 0x31 || Opcode == 0x33 || Opcode == 0x29 || Opcode == 0x2B) && !Rm.bMemory && Rm.Register == Reg)
                {
                    Discard(Reg);
                }
                else if (!bTwoByte && Opcode >= 0x58 && Opcode <= 0x5F)
                {
                    Discard((Opcode - 0x58) | (Instruction.rex_b << 3));
                }
                else if (!bTwoByte && Opcode == 0xFF && (Instruction.modrm_reg == 2 || Instruction.modrm_reg == 4))
                {
                    if (Rm.bMemory && Rm.Base >= 0 && (State.VTables & (1u << Rm.Base)) && (State.Held & (1u << Rcx)))
                    {
                        Uses.push_back({ FUse::VirtualCall, static_cast<uintptr_t>(static_cast<uint32_t>(Rm.Displacement) / 8) });
                    }
                    if (Instruction.modrm_reg == 4)
                    {
                        break;
                    }
                    State.Held &= ~VolatileRegisters;
                    State.VTables &= ~VolatileRegisters;
                }
                else if (!bTwoByte && Opcode == 0xE8)
                {
                    if (State.Held & (1u << Rcx))
                    {
                        Uses.push_back({ FUse::Call, Finder::RelativeTarget(Address, 1, 5) });
                    }
                    State.Held &= ~VolatileRegisters;
                    State.VTables &= ~VolatileRegisters;
                }
                else if (!bTwoByte && (Opcode == 0xC3 || Opcode == 0xC2))
                {
                    break;
                }
                else if (!bTwoByte && (Opcode == 0xE9 || Opcode == 0xEB))
                {
                    const uintptr_t Target = Opcode == 0xE9 ? Finder::RelativeTarget(Address, 1, 5) : Next + static_cast<int8_t>(Instruction.imm.imm8);
                    if (Target >= FunctionBegin && Target < FunctionEnd)
                    {
                        Address = Target;
                        continue;
                    }
                    if (State.Held & (1u << Rcx))
                    {
                        Uses.push_back({ FUse::Call, Target });
                    }
                    break;
                }
                else if (!bTwoByte && Opcode >= 0x70 && Opcode <= 0x7F)
                {
                    Work.push_back({ Next + static_cast<int8_t>(Instruction.imm.imm8), State });
                }
                else if (bTwoByte && Instruction.opcode2 >= 0x80 && Instruction.opcode2 <= 0x8F)
                {
                    Work.push_back({ Next + static_cast<int32_t>(Instruction.imm.imm32), State });
                }

                Address = Next;
            }
        }
        return Uses;
    }

    FVotes Footprint(FContext& Context, uintptr_t Function, int32_t Depth);

    FVotes Vote(FContext& Context, const std::vector<FUse>& Uses, int32_t Depth)
    {
        FVotes Votes;
        for (const FUse& Use : Uses)
        {
            if (Use.Kind == FUse::Offset)
            {
                if (Use.Value >= static_cast<uintptr_t>(Context.DirectorSize) && Use.Value < 0x2000)
                {
                    ++Votes.Other;
                }
                else if (Use.Value >= static_cast<uintptr_t>(Context.GoalManagerSize) && Use.Value < 0x2000)
                {
                    ++Votes.Director;
                }
                else if (Use.Value >= static_cast<uintptr_t>(Context.ActorSize) && Use.Value < static_cast<uintptr_t>(Context.GoalManagerSize))
                {
                    ++Votes.GoalManager;
                }
            }
            else if (Use.Kind == FUse::VirtualCall)
            {
                const int32_t Slot = static_cast<int32_t>(Use.Value);
                if (Slot >= Context.ActorSlots && Slot < 0x200)
                {
                    const uintptr_t DirectorEntry = Slot < Context.DirectorSlots ? VTableAt(Context.DirectorVTable, Slot) : 0;
                    if (DirectorEntry && !IsTrivial(DirectorEntry))
                    {
                        ++Votes.Director;
                    }
                    else
                    {
                        ++Votes.Other;
                    }
                    continue;
                }
                if (Slot >= 0x200 || Depth >= 3)
                {
                    continue;
                }
                const uintptr_t GoalManagerEntry = VTableAt(Context.GoalManagerVTable, Slot);
                const uintptr_t DirectorEntry = VTableAt(Context.DirectorVTable, Slot);
                if (!GoalManagerEntry || !DirectorEntry || GoalManagerEntry == DirectorEntry)
                {
                    continue;
                }
                const FVotes GoalManagerFoot = IsTrivial(GoalManagerEntry) ? FVotes{} : Footprint(Context, GoalManagerEntry, Depth + 1);
                const FVotes DirectorFoot = IsTrivial(DirectorEntry) ? FVotes{} : Footprint(Context, DirectorEntry, Depth + 1);
                if ((DirectorFoot.Director || DirectorFoot.Other) && !DirectorFoot.GoalManager && !GoalManagerFoot.GoalManager)
                {
                    ++(DirectorFoot.Other ? Votes.Other : Votes.Director);
                }
                else if (GoalManagerFoot.GoalManager && !GoalManagerFoot.Director && !GoalManagerFoot.Other && !DirectorFoot.Director && !DirectorFoot.Other)
                {
                    ++Votes.GoalManager;
                }
            }
            else if (Use.Value != Context.Stub && Depth < 3)
            {
                const FVotes Callee = Footprint(Context, Use.Value, Depth + 1);
                if (Callee.GoalManager && !Callee.Director && !Callee.Other)
                {
                    ++Votes.GoalManager;
                }
                else if (Callee.Director && !Callee.GoalManager && !Callee.Other)
                {
                    ++Votes.Director;
                }
                else if (Callee.Other && !Callee.GoalManager)
                {
                    ++Votes.Other;
                }
            }
        }
        return Votes;
    }

    FVotes Footprint(FContext& Context, uintptr_t Function, int32_t Depth)
    {
        if (const auto Found = Context.Footprints.find(Function); Found != Context.Footprints.end())
        {
            return Found->second;
        }
        Context.Footprints[Function] = {};
        if (Function < Finder::TextBegin() || Function >= Finder::TextEnd())
        {
            return {};
        }
        const uintptr_t End = Finder::FunctionStart(Function) == Function ? Finder::FunctionEnd(Function) : Function + 0x400;
        const auto Result = Vote(Context, Walk(Function, Function, End, Rcx), Depth);
        Context.Footprints[Function] = Result;
        return Result;
    }
}

std::vector<uint8_t*> Finder::GoalManagerGetCurrentCalls(void** GoalManagerVTable, void** DirectorVTable, int32_t GoalManagerSize, int32_t DirectorSize, int32_t ActorSize, int32_t GetWorldIndex, uintptr_t& OutStub, std::vector<uint8_t*>& OutDirectorCalls)
{
    const auto StartTime = std::chrono::steady_clock::now();
    OutStub = 0;
    OutDirectorCalls.clear();
    std::vector<uint8_t*> Result;
    const auto AnchorReferences = Finder::StringRefs(L"%s failed due to NULL WorldContextObject.  Without WorldContextObject, cannot find Goal Manager!  Assignment Identifier: %s.");
    const uintptr_t Anchor = AnchorReferences.empty() ? 0 : Finder::FunctionStart(AnchorReferences.front());
    const uintptr_t AnchorEnd = Anchor ? Finder::FunctionEnd(Anchor) : 0;

    FContext Context;
    for (const auto& [Call, Target] : Anchor ? Finder::DirectCalls(Anchor, AnchorEnd) : std::vector<std::pair<uintptr_t, uintptr_t>>{})
    {
        const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Target);
        if (!Context.Stub && Bytes[0] == 0x33 && Bytes[1] == 0xC0 && Bytes[2] == 0xC3)
        {
            Context.Stub = Target;
        }
    }
    if (!Context.Stub || !GoalManagerVTable || !DirectorVTable || GoalManagerSize <= ActorSize || DirectorSize <= GoalManagerSize)
    {
        Log::Error(L"Finder: AFortAIGoalManager::GetCurrent call sites: inputs not found");
        return Result;
    }

    OutStub = Context.Stub;
    Context.GoalManagerVTable = GoalManagerVTable;
    Context.DirectorVTable = DirectorVTable;
    Context.GoalManagerSize = GoalManagerSize;
    Context.DirectorSize = DirectorSize;
    Context.ActorSize = ActorSize;
    while (Context.DirectorSlots < 0x200 && VTableAt(DirectorVTable, Context.DirectorSlots))
    {
        ++Context.DirectorSlots;
    }
    for (int32_t Slot = 0; Slot < 0x200; ++Slot)
    {
        const uintptr_t Entry = VTableAt(GoalManagerVTable, Slot);
        const uintptr_t DirectorEntry = VTableAt(DirectorVTable, Slot);
        if (!Entry || !DirectorEntry)
        {
            // The end of either vtable: what follows belongs to other classes (1.7.2 matches again at 0x1E0-0x1FD).
            break;
        }
        if (Entry == DirectorEntry && !IsTrivial(Entry))
        {
            Context.ActorSlots = Slot + 1;
        }
    }

    const uintptr_t ImageBase = Finder::ImageBase();
    const uintptr_t TextEnd = Finder::TextEnd();
    for (uintptr_t Site = Finder::TextBegin(); Site + 5 <= TextEnd; ++Site)
    {
        if (*reinterpret_cast<const uint8_t*>(Site) != 0xE8 || Finder::RelativeTarget(Site, 1, 5) != Context.Stub)
        {
            continue;
        }
        const uintptr_t Begin = Finder::FunctionStart(Site);
        const uintptr_t End = Begin ? Finder::FunctionEnd(Begin) : 0;
        if (!Begin || !End || Site >= End)
        {
            continue;
        }

        FVotes Votes = Vote(Context, Walk(Site + 5, Begin, End, 0), 0);
        if (Begin == Anchor)
        {
            ++Votes.GoalManager;
        }
        if (Votes.GoalManager && !Votes.Director && !Votes.Other)
        {
            Result.push_back(reinterpret_cast<uint8_t*>(Site));
            Log::Info(L"Finder: AFortAIGoalManager::GetCurrent call site +0x{:X} in +0x{:X}", Site - ImageBase, Begin - ImageBase);
        }
        else if (Votes.Director && !Votes.GoalManager && !Votes.Other && IsGetCurrentOfWorld(Site, GetWorldIndex))
        {
            OutDirectorCalls.push_back(reinterpret_cast<uint8_t*>(Site));
            Log::Info(L"Finder: AFortAIDirector::GetCurrent call site +0x{:X} in +0x{:X}", Site - ImageBase, Begin - ImageBase);
        }
    }
    const auto Milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - StartTime).count();
    Log::Info(L"Finder: {} AFortAIGoalManager::GetCurrent and {} AFortAIDirector::GetCurrent call sites classified in {} ms", Result.size(), OutDirectorCalls.size(), Milliseconds);
    return Result;
}
