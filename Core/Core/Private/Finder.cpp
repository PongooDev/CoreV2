#include "pch.h"
#include "Finder.h"
#include "Log.h"

namespace
{
    Memcury::PE::Section TextSection()
    {
        return Memcury::PE::Section::GetSection(".text");
    }
}

uintptr_t Finder::ImageBase()
{
    return Memcury::PE::GetModuleBase();
}

uintptr_t Finder::TextBegin()
{
    return TextSection().GetSectionStart().Get();
}

uintptr_t Finder::TextEnd()
{
    return TextSection().GetSectionEnd().Get();
}

std::vector<uintptr_t> Finder::StringRefs(const wchar_t* Text)
{
    std::vector<uintptr_t> Result;
    for (Memcury::Scanner& Scan : Memcury::Scanner::FindStringRefs<const wchar_t*>(Text))
    {
        Result.push_back(Scan.Get());
    }
    return Result;
}

std::vector<uintptr_t> Finder::StringRefs(const char* Text)
{
    std::vector<uintptr_t> Result;
    for (Memcury::Scanner& Scan : Memcury::Scanner::FindStringRefs<const char*>(Text))
    {
        Result.push_back(Scan.Get());
    }
    return Result;
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

    for (auto* Import = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(Base + Directory.VirtualAddress); Import->Name; ++Import)
    {
        const char* Name = reinterpret_cast<const char*>(Base + Import->Name);
        if (_stricmp(Name, Module) != 0)
        {
            continue;
        }

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
