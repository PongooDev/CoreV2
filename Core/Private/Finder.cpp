#include "pch.h"
#include "Finder.h"
#include "Log.h"
#include "../../ThirdParty/MinHook/src/hde/hde64.h"

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <unordered_map>

namespace
{
    Memcury::PE::Section TextSection()
    {
        return Memcury::PE::Section::GetSection(".text");
    }

    struct FStringReference
    {
        uint32_t Target;
        uint32_t Site;
    };

    const std::vector<FStringReference>& StringReferences()
    {
        static const std::vector<FStringReference> References = []
        {
            std::vector<FStringReference> Found;
            Memcury::PE::Section ReadOnly = Memcury::PE::Section::GetSection(".rdata");
            const uintptr_t Base = Memcury::PE::GetModuleBase();
            const uintptr_t ReadOnlyBegin = ReadOnly.GetSectionStart().Get();
            const uintptr_t ReadOnlyEnd = ReadOnly.GetSectionEnd().Get();
            // .text's references come first; 1.9.1's moved code holds others (Finder::HasMovedCode).
            for (const auto& [Begin, End] : Finder::ExecutableSections())
            {
                const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Begin);
                const size_t Size = End - Begin;
                for (size_t Index = 0; Index + 7 <= Size; ++Index)
                {
                    if ((Bytes[Index] != 0x48 && Bytes[Index] != 0x4C) || Bytes[Index + 1] != 0x8D || (Bytes[Index + 2] & 0xC7) != 0x05)
                    {
                        continue;
                    }

                    const uintptr_t Site = reinterpret_cast<uintptr_t>(Bytes + Index);
                    const uintptr_t Target = Site + 7 + *reinterpret_cast<const int32_t*>(Bytes + Index + 3);
                    if (Target >= ReadOnlyBegin && Target < ReadOnlyEnd)
                    {
                        Found.push_back({ static_cast<uint32_t>(Target - Base), static_cast<uint32_t>(Site - Base) });
                    }
                }
            }
            return Found;
        }();
        return References;
    }

    template <typename TChar>
    std::vector<uintptr_t> FindStringReferences(const TChar* Text)
    {
        std::vector<uintptr_t> Result;
        const uintptr_t Base = Memcury::PE::GetModuleBase();
        for (const FStringReference& Reference : StringReferences())
        {
            const TChar* Candidate = reinterpret_cast<const TChar*>(Base + Reference.Target);
            if (Candidate[0] != Text[0])
            {
                continue;
            }

            bool bMatch = false;
            if constexpr (sizeof(TChar) == sizeof(wchar_t))
            {
                bMatch = wcscmp(Candidate, Text) == 0;
            }
            else
            {
                bMatch = strcmp(Candidate, Text) == 0;
            }

            if (bMatch)
            {
                Result.push_back(Base + Reference.Site);
            }
        }
        return Result;
    }
}

const std::vector<std::pair<uintptr_t, uintptr_t>>& Finder::ExecutableSections()
{
    static const std::vector<std::pair<uintptr_t, uintptr_t>> Sections = []
    {
        Memcury::PE::Section Text = TextSection();
        std::vector<std::pair<uintptr_t, uintptr_t>> Found = { { Text.GetSectionStart().Get(), Text.GetSectionEnd().Get() } };
        const uintptr_t Base = Memcury::PE::GetModuleBase();
        const IMAGE_NT_HEADERS* Headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(Base + reinterpret_cast<const IMAGE_DOS_HEADER*>(Base)->e_lfanew);
        const IMAGE_SECTION_HEADER* Section = IMAGE_FIRST_SECTION(Headers);
        for (WORD Index = 0; Index < Headers->FileHeader.NumberOfSections; ++Index, ++Section)
        {
            const uintptr_t Begin = Base + Section->VirtualAddress;
            if ((Section->Characteristics & IMAGE_SCN_MEM_EXECUTE) && Begin != Found.front().first)
            {
                Found.push_back({ Begin, Begin + Section->Misc.VirtualSize });
            }
        }
        return Found;
    }();
    return Sections;
}

bool Finder::HasMovedCode()
{
    // 1.1 to 1.9 have one executable section. 1.9.1's exe has a second one (also named .text) that holds the bodies of
    // about 1,300 functions moved out of the first: their starts there jump to bodies split into pieces joined by jumps,
    // with no .pdata, whose calls, branches and returns are rewritten (Core-Docs/Build-1.9.1-Audit.md).
    return ExecutableSections().size() > 1;
}

namespace
{
    bool IsExecutable(uintptr_t Address)
    {
        for (const auto& [Begin, End] : Finder::ExecutableSections())
        {
            if (Address >= Begin && Address + 16 <= End)
            {
                return true;
            }
        }
        return false;
    }

    // push r64; lea r64, [rip+X]; xchg r64, [rsp]; ret: a moved function's jump to X.
    uintptr_t PushReturnTarget(const uint8_t* Code)
    {
        size_t At = 0;
        const bool bHighPush = Code[At] == 0x41;
        At += bHighPush ? 1 : 0;
        if (Code[At] < 0x50 || Code[At] > 0x57)
        {
            return 0;
        }
        const uint8_t Register = static_cast<uint8_t>((Code[At] - 0x50) | (bHighPush ? 8 : 0));
        ++At;
        const uint8_t Rex = static_cast<uint8_t>(0x48 | (Register >= 8 ? 0x04 : 0x00));
        const uint8_t ModRm = static_cast<uint8_t>(0x05 | ((Register & 7) << 3));
        if (Code[At] != Rex || Code[At + 1] != 0x8D || Code[At + 2] != ModRm)
        {
            return 0;
        }
        const uintptr_t Target = reinterpret_cast<uintptr_t>(Code + At) + 7 + *reinterpret_cast<const int32_t*>(Code + At + 3);
        At += 7;
        if (Code[At] != Rex || Code[At + 1] != 0x87 || Code[At + 2] != static_cast<uint8_t>(0x04 | ((Register & 7) << 3)) || Code[At + 3] != 0x24 || Code[At + 4] != 0xC3)
        {
            return 0;
        }
        return Target;
    }

    // ret, ret 0, or moved code's lea rsp, [rsp+8]; jmp qword ptr [rsp-8].
    bool IsReturn(const uint8_t* Code)
    {
        static constexpr uint8_t Moved[] = { 0x48, 0x8D, 0x64, 0x24, 0x08, 0xFF, 0x64, 0x24, 0xF8 };
        return Code[0] == 0xC3 || (Code[0] == 0xC2 && Code[1] == 0x00 && Code[2] == 0x00) || std::equal(std::begin(Moved), std::end(Moved), Code);
    }
}

uintptr_t Finder::Unthunk(uintptr_t Address)
{
    for (int32_t Hop = 0; HasMovedCode() && Hop < 32 && IsExecutable(Address); ++Hop)
    {
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Address);
        uintptr_t Next = 0;
        if (Code[0] == 0xE9)
        {
            Next = Address + 5 + *reinterpret_cast<const int32_t*>(Code + 1);
        }
        else if (Code[0] == 0xEB)
        {
            Next = Address + 2 + static_cast<int8_t>(Code[1]);
        }
        else
        {
            Next = PushReturnTarget(Code);
        }
        if (!Next || !IsExecutable(Next))
        {
            break;
        }
        Address = Next;
    }
    return Address;
}

bool Finder::BodyIs(uintptr_t Function, std::initializer_list<uint8_t> Bytes)
{
    const bool bInText = Function >= TextBegin() && Function + Bytes.size() <= TextEnd();
    if (!Function || !Bytes.size() || (!bInText && !IsReadable(reinterpret_cast<const void*>(Function), Bytes.size())))
    {
        return false;
    }
    if (std::equal(Bytes.begin(), Bytes.end(), reinterpret_cast<const uint8_t*>(Function)))
    {
        return true;
    }
    if (!HasMovedCode())
    {
        return false;
    }

    // Moved code rewrites a stub's return and puts its body behind jumps: compare what comes before the return.
    size_t Length = Bytes.size();
    if (Bytes.end()[-1] == 0xC3)
    {
        Length -= 1;
    }
    else if (Length >= 3 && Bytes.end()[-3] == 0xC2 && Bytes.end()[-2] == 0x00 && Bytes.end()[-1] == 0x00)
    {
        Length -= 3;
    }
    else
    {
        return false;
    }
    const uintptr_t Body = Unthunk(Function);
    if (!IsExecutable(Body) || !std::equal(Bytes.begin(), Bytes.begin() + Length, reinterpret_cast<const uint8_t*>(Body)))
    {
        return false;
    }
    const uintptr_t Return = Unthunk(Body + Length);
    return IsExecutable(Return) && IsReturn(reinterpret_cast<const uint8_t*>(Return));
}

uintptr_t Finder::TraceFind(uintptr_t From, size_t MaxInstructions, const std::function<bool(uintptr_t Instruction)>& Match)
{
    uintptr_t Address = From;
    for (size_t Count = 0; Address && Count < MaxInstructions && IsExecutable(Address); ++Count)
    {
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Address);
        if (Code[0] == 0xE9 || Code[0] == 0xEB || PushReturnTarget(Code))
        {
            const uintptr_t Next = Unthunk(Address);
            if (Next == Address)
            {
                return 0;
            }
            Address = Next;
            continue;
        }
        if (Match(Address))
        {
            return Address;
        }
        hde64s Instruction{};
        const uint32_t Length = hde64_disasm(Code, &Instruction);
        if (!Length || (Instruction.flags & F_ERROR) || Code[0] == 0xC3 || Code[0] == 0xC2 || Code[0] == 0xCC || (Code[0] == 0xFF && (Instruction.modrm_reg == 4 || Instruction.modrm_reg == 5)))
        {
            return 0;
        }
        Address += Length;
    }
    return 0;
}

int32_t Finder::RunsInto(uintptr_t From, uintptr_t Goal, size_t MaxInstructions)
{
    // Every way on from each instruction: both sides of a branch, and the code addresses moved code loads (mov r64,
    // imm64 or lea r64, [rip+X]) to branch or jump through the stack. Calls return to the next instruction.
    std::unordered_map<uintptr_t, int32_t> Steps = { { From, 0 } };
    std::vector<uintptr_t> Queue = { From };
    for (size_t Next = 0; Next < Queue.size() && Next < MaxInstructions; ++Next)
    {
        const uintptr_t Address = Queue[Next];
        const int32_t Step = Steps[Address];
        if (Address == Goal)
        {
            return Step;
        }
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Address);
        hde64s Instruction{};
        const uint32_t Length = IsExecutable(Address) ? hde64_disasm(Code, &Instruction) : 0;
        if (!Length || (Instruction.flags & F_ERROR))
        {
            continue;
        }

        const uintptr_t After = Address + Length;
        const uint8_t Opcode = static_cast<uint8_t>(Instruction.opcode);
        const bool bShortBranch = Opcode == 0xEB || (Opcode >= 0x70 && Opcode <= 0x7F);
        const bool bNearBranch = Opcode == 0xE9 || (Opcode == 0x0F && Instruction.opcode2 >= 0x80 && Instruction.opcode2 <= 0x8F);
        const bool bEnd = Opcode == 0xC3 || Opcode == 0xC2 || Opcode == 0xCC || (Opcode == 0xFF && (Instruction.modrm_reg == 4 || Instruction.modrm_reg == 5));
        uintptr_t Ways[2] = { bEnd || Opcode == 0xE9 || Opcode == 0xEB ? 0 : After, 0 };
        if (bShortBranch)
        {
            Ways[1] = After + static_cast<int8_t>(Instruction.imm.imm8);
        }
        else if (bNearBranch)
        {
            Ways[1] = After + static_cast<int32_t>(Instruction.imm.imm32);
        }
        else if (Instruction.rex_w && Opcode >= 0xB8 && Opcode <= 0xBF)
        {
            Ways[1] = static_cast<uintptr_t>(Instruction.imm.imm64);
        }
        else if (Instruction.rex_w && Opcode == 0x8D && Instruction.modrm_mod == 0 && Instruction.modrm_rm == 5)
        {
            Ways[1] = After + static_cast<int32_t>(Instruction.disp.disp32);
        }
        for (const uintptr_t Way : Ways)
        {
            if (Way && IsExecutable(Way) && Steps.emplace(Way, Step + 1).second)
            {
                Queue.push_back(Way);
            }
        }
    }
    return -1;
}

uintptr_t Finder::MostReferencedFunction()
{
    // The shared empty function is what most vtable slots hold: on 1.9.1 76,716 .rdata pointers (the next, a
    // return-zero stub, 26,127). Its body there is a jump into the second section's guard code.
    static const uintptr_t Most = []
    {
        Memcury::PE::Section ReadOnly = Memcury::PE::Section::GetSection(".rdata");
        const auto* Begin = reinterpret_cast<const uintptr_t*>(ReadOnly.GetSectionStart().Get());
        const auto* End = reinterpret_cast<const uintptr_t*>(ReadOnly.GetSectionEnd().Get() & ~static_cast<uintptr_t>(7));
        std::unordered_map<uintptr_t, uint32_t> Counts;
        Counts.reserve(1 << 18);
        uintptr_t Best = 0;
        uint32_t BestCount = 0;
        for (const uintptr_t* Slot = Begin; Slot < End; ++Slot)
        {
            if (*Slot >= TextBegin() && *Slot < TextEnd())
            {
                const uint32_t Count = ++Counts[*Slot];
                if (Count > BestCount)
                {
                    Best = *Slot;
                    BestCount = Count;
                }
            }
        }
        return Best;
    }();
    return Most;
}

uintptr_t Finder::ImageBase()
{
    return Memcury::PE::GetModuleBase();
}

uintptr_t Finder::TextBegin()
{
    static const uintptr_t Begin = TextSection().GetSectionStart().Get();
    return Begin;
}

uintptr_t Finder::TextEnd()
{
    static const uintptr_t End = TextSection().GetSectionEnd().Get();
    return End;
}

std::vector<uintptr_t> Finder::StringRefs(const wchar_t* Text)
{
    return FindStringReferences(Text);
}

std::vector<uintptr_t> Finder::StringRefs(const char* Text)
{
    return FindStringReferences(Text);
}

uintptr_t Finder::FindReadOnly(const void* Bytes, size_t Size)
{
    Memcury::PE::Section ReadOnly = Memcury::PE::Section::GetSection(".rdata");
    const uint8_t* const Begin = reinterpret_cast<const uint8_t*>(ReadOnly.GetSectionStart().Get());
    const uint8_t* const End = reinterpret_cast<const uint8_t*>(ReadOnly.GetSectionEnd().Get());
    const uint8_t* const Found = Size && End - Begin >= static_cast<ptrdiff_t>(Size) ? std::search(Begin, End, static_cast<const uint8_t*>(Bytes), static_cast<const uint8_t*>(Bytes) + Size) : End;
    return Found != End ? reinterpret_cast<uintptr_t>(Found) : 0;
}

uintptr_t Finder::FunctionStart(uintptr_t Address)
{
    if (!Address)
    {
        return 0;
    }
    return Memcury::Scanner(Memcury::PE::Address(Address)).FindFunctionStart().Get();
}

uintptr_t Finder::FunctionEnd(uintptr_t Start)
{
    if (!Start)
    {
        return 0;
    }
    return Memcury::Scanner(Memcury::PE::Address(Start)).FindFunctionEnd().Get();
}

uintptr_t Finder::RelativeTarget(uintptr_t Instruction, uint32_t OperandOffset, uint32_t InstructionLength)
{
    return Instruction + InstructionLength + *reinterpret_cast<const int32_t*>(Instruction + OperandOffset);
}

uintptr_t Finder::NextCall(uintptr_t From, size_t MaxDistance)
{
    if (!From)
    {
        return 0;
    }
    Memcury::Scanner Call = Memcury::Scanner(Memcury::PE::Address(From)).FindNextCall(MaxDistance);
    if (!Call.IsValid())
    {
        return 0;
    }
    return Call.GetRelativeTarget(1, 5).Get();
}

std::vector<std::pair<uintptr_t, uintptr_t>> Finder::DirectCalls(uintptr_t Begin, uintptr_t End)
{
    return Memcury::Scanner::FindDirectCalls(Begin, End);
}

std::vector<uintptr_t> Finder::CallsTo(uintptr_t Target, bool bIncludeJumps)
{
    std::vector<uintptr_t> Result;
    for (Memcury::Scanner& Scan : Memcury::Scanner::FindCallsTo(Target, bIncludeJumps))
    {
        Result.push_back(Scan.Get());
    }
    return Result;
}

int32_t Finder::VTableIndex(void** VTable, void* Function, int32_t MaxSlots)
{
    if (!VTable || !Function)
    {
        return -1;
    }

    const uintptr_t Begin = TextSection().GetSectionStart().Get();
    const uintptr_t End = TextSection().GetSectionEnd().Get();

    int32_t Found = -1;
    for (int32_t Index = 0; Index < MaxSlots; ++Index)
    {
        const uintptr_t Entry = reinterpret_cast<uintptr_t>(VTable[Index]);
        if (Entry < Begin || Entry >= End)
        {
            break;
        }
        if (VTable[Index] == Function)
        {
            if (Found != -1)
            {
                return -1;
            }
            Found = Index;
        }
    }
    return Found;
}

void** Finder::VTableContaining(uintptr_t Function, int32_t Slot, int32_t Slots)
{
    if (!Function)
    {
        return nullptr;
    }

    Memcury::PE::Section ReadOnly = Memcury::PE::Section::GetSection(".rdata");
    const uintptr_t Begin = ReadOnly.GetSectionStart().Get();
    const uintptr_t End = ReadOnly.GetSectionEnd().Get();
    const uintptr_t CodeBegin = TextBegin();
    const uintptr_t CodeEnd = TextEnd();

    void** Found = nullptr;
    for (uintptr_t At = (Begin + 7) & ~static_cast<uintptr_t>(7); At + sizeof(uintptr_t) <= End; At += sizeof(uintptr_t))
    {
        if (*reinterpret_cast<const uintptr_t*>(At) != Function || At < Begin + Slot * sizeof(uintptr_t))
        {
            continue;
        }

        void** VTable = reinterpret_cast<void**>(At - Slot * sizeof(uintptr_t));
        if (reinterpret_cast<uintptr_t>(VTable + Slots) > End)
        {
            continue;
        }

        bool bCode = true;
        for (int32_t Index = 0; Index < Slots && bCode; ++Index)
        {
            const uintptr_t Entry = reinterpret_cast<uintptr_t>(VTable[Index]);
            bCode = Entry >= CodeBegin && Entry < CodeEnd;
        }
        if (!bCode)
        {
            continue;
        }
        if (Found)
        {
            return nullptr;
        }
        Found = VTable;
    }
    return Found;
}

uintptr_t Finder::ImportSlot(const char* Module, const char* Symbol)
{
    const uintptr_t Base = ImageBase();
    const auto* DosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
    const auto* NtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(Base + DosHeader->e_lfanew);
    const IMAGE_DATA_DIRECTORY& Directory = NtHeaders->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!Directory.VirtualAddress)
    {
        return 0;
    }

    const auto* First = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(Base + Directory.VirtualAddress);

    for (auto* Import = First; Import->Name; ++Import)
    {
        const char* Name = reinterpret_cast<const char*>(Base + Import->Name);
        if (_stricmp(Name, Module) != 0)
        {
            continue;
        }

        if (!Import->OriginalFirstThunk) continue;
        const auto* Thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(Base + Import->OriginalFirstThunk);
        auto* Entry = reinterpret_cast<const IMAGE_THUNK_DATA*>(Base + Import->FirstThunk);
        for (; Thunk->u1.AddressOfData; ++Thunk, ++Entry)
        {
            if (Thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
            {
                continue;
            }
            const auto* ByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(Base + Thunk->u1.AddressOfData);
            if (strcmp(ByName->Name, Symbol) == 0)
            {
                return reinterpret_cast<uintptr_t>(Entry);
            }
        }
    }

    // Fallback: the import directory may name a DLL differently than the caller
    // (API-set vs. host name, or a function forwarded to another DLL).  Search
    // every descriptor for the symbol by name.
    for (auto* Import = First; Import->Name; ++Import)
    {
        if (!Import->OriginalFirstThunk) continue;
        const auto* Thunk = reinterpret_cast<const IMAGE_THUNK_DATA*>(Base + Import->OriginalFirstThunk);
        auto* Entry = reinterpret_cast<const IMAGE_THUNK_DATA*>(Base + Import->FirstThunk);
        for (; Thunk->u1.AddressOfData; ++Thunk, ++Entry)
        {
            if (Thunk->u1.Ordinal & IMAGE_ORDINAL_FLAG)
            {
                continue;
            }
            const auto* ByName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(Base + Thunk->u1.AddressOfData);
            if (strcmp(ByName->Name, Symbol) == 0)
            {
                return reinterpret_cast<uintptr_t>(Entry);
            }
        }
    }

    return 0;
}

bool Finder::IsReadable(const void* Pointer, size_t Size)
{
    if (!Pointer)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION Info = {};
    const auto* Cursor = static_cast<const uint8_t*>(Pointer);
    const uint8_t* End = Cursor + Size;

    while (Cursor < End)
    {
        if (!VirtualQuery(Cursor, &Info, sizeof(Info)))
        {
            return false;
        }

        const DWORD Readable = PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_WRITECOPY;
        if (Info.State != MEM_COMMIT || (Info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) || !(Info.Protect & Readable))
        {
            return false;
        }

        Cursor = static_cast<const uint8_t*>(Info.BaseAddress) + Info.RegionSize;
    }
    return true;
}
