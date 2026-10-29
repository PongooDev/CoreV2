#include "pch.h"
#include "Finder.h"
#include "Log.h"

#include <algorithm>
#include <cstring>
#include <cwchar>

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
            Memcury::PE::Section Text = TextSection();
            Memcury::PE::Section ReadOnly = Memcury::PE::Section::GetSection(".rdata");
            const uintptr_t Base = Memcury::PE::GetModuleBase();
            const uintptr_t ReadOnlyBegin = ReadOnly.GetSectionStart().Get();
            const uintptr_t ReadOnlyEnd = ReadOnly.GetSectionEnd().Get();
            const uint8_t* Bytes = reinterpret_cast<const uint8_t*>(Text.GetSectionStart().Get());
            const size_t Size = Text.GetSectionSize();
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
