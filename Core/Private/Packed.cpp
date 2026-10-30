#include "pch.h"
#include "Packed.h"
#include "Log.h"
#include "Natives.h"

namespace
{
    /// <summary>The opening of an x64 function with a frame, `mov [rsp+8], rbx`. Decrypted game code is full of
    /// them and ciphertext has them only by chance, which is what tells the two apart.</summary>
    constexpr uint8_t Prologue[] = { 0x48, 0x89, 0x5C, 0x24 };
    constexpr int PrologueThreshold = 8;
    constexpr size_t SampleSize = 0x10000;

    /// <summary>
    /// The entry point is a wrapper the packer put there: `sub rsp,28 / call stub / add rsp,28 / jmp CRT`. The stub
    /// decrypts the half after its own call last of all, so these bytes turning up is the handover itself, and a
    /// more exact signal than the image merely looking mostly decrypted.
    /// </summary>
    constexpr uint8_t Handover[] = { 0x48, 0x83, 0xC4, 0x28, 0xE9 };
    constexpr size_t HandoverOffset = 9;

    constexpr DWORD TimeoutMs = 60000;
    constexpr DWORD CheckTimeoutMs = 300000;
    constexpr DWORD PollMs = 2;

    struct FImage
    {
        const uint8_t* Base = nullptr;
        const uint8_t* EntryPoint = nullptr;
        const uint8_t* Text = nullptr;
        size_t TextSize = 0;
        int TextSections = 0;
    };

    FImage Describe()
    {
        FImage Image;
        const auto* Base = reinterpret_cast<const uint8_t*>(GetModuleHandleW(nullptr));
        if (!Base)
        {
            return Image;
        }

        const auto* DosHeader = reinterpret_cast<const IMAGE_DOS_HEADER*>(Base);
        if (DosHeader->e_magic != IMAGE_DOS_SIGNATURE)
        {
            return Image;
        }

        const auto* NtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS64*>(Base + DosHeader->e_lfanew);
        if (NtHeaders->Signature != IMAGE_NT_SIGNATURE)
        {
            return Image;
        }

        Image.Base = Base;
        Image.EntryPoint = Base + NtHeaders->OptionalHeader.AddressOfEntryPoint;

        const auto* Section = IMAGE_FIRST_SECTION(NtHeaders);
        for (WORD Index = 0; Index < NtHeaders->FileHeader.NumberOfSections; ++Index, ++Section)
        {
            if (memcmp(Section->Name, ".text", 6) != 0)
            {
                continue;
            }

            // Only the first .text holds the game; the second is the stub's own.
            if (++Image.TextSections == 1)
            {
                Image.Text = Base + Section->VirtualAddress;
                Image.TextSize = Section->SizeOfRawData;
            }
        }
        return Image;
    }

    bool LooksDecrypted(const FImage& Image)
    {
        if (!Image.Text || Image.TextSize < sizeof(Prologue))
        {
            return false;
        }

        // Sample well inside .text: its first pages are reached before the rest and would say "decrypted" early.
        const size_t Length = Image.TextSize < SampleSize ? Image.TextSize : SampleSize;
        const size_t Room = Image.TextSize - Length;
        const size_t Offset = Room < 0x100000 ? Room : 0x100000;
        const uint8_t* Sample = Image.Text + Offset;

        int Count = 0;
        for (size_t At = 0; At + sizeof(Prologue) <= Length; ++At)
        {
            if (memcmp(Sample + At, Prologue, sizeof(Prologue)) == 0 && ++Count >= PrologueThreshold)
            {
                return true;
            }
        }
        return false;
    }

    bool HandedOver(const FImage& Image)
    {
        return Image.EntryPoint
            && memcmp(Image.EntryPoint + HandoverOffset, Handover, sizeof(Handover)) == 0;
    }

    void (*GStart)() = nullptr;

    DWORD WINAPI Watch(LPVOID)
    {
        const FImage Image = Describe();
        const DWORD Deadline = GetTickCount() + TimeoutMs;

        while (!HandedOver(Image) || !LooksDecrypted(Image))
        {
            if (GetTickCount() > Deadline)
            {
                Log::Error(L"Packed exe: the stub did not hand over within {}s, starting anyway", TimeoutMs / 1000);
                break;
            }
            Sleep(PollMs);
        }

        // Deliberately not holding the main thread still while Core starts. The handover means the stub has
        // finished, so what runs next is the CRT's own start-up, and everything Core hooks belongs to the engine
        // and is not reached until engine init. Suspending a thread that may hold the heap or loader lock while
        // Core allocates and patches is a real deadlock, and buys very little.
        Log::Info(L"Packed exe: the stub has handed over, starting Core");
        GStart();
        return 0;
    }
}

bool Packed::RunAfterStub(void (*Start)())
{
    const FImage Image = Describe();
    if (Image.TextSections < 2 || LooksDecrypted(Image))
    {
        return false;
    }

    Log::Info(L"Packed exe detected: waiting for the packer's stub to decrypt the image");
    GStart = Start;

    const HANDLE Thread = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr);
    if (!Thread)
    {
        Log::Error(L"Packed exe: could not start the watcher, starting Core now instead");
        return false;
    }

    CloseHandle(Thread);
    return true;
}

bool Packed::IsPacked()
{
    return Describe().TextSections >= 2;
}

void Packed::WaitForCodeCheck()
{
    if (Describe().TextSections < 2)
    {
        return;
    }

    // GEngine is set in FEngineLoop::Init, the step after the initial load whose end the packer checks at, so the
    // engine existing means the check has been and gone. Waiting on a timer instead would be guesswork: the window
    // is a phase of start-up, and how long the phase takes moves around between runs.
    if (!Natives::GEngine)
    {
        Log::Error(L"Packed exe: GEngine was never resolved, so enabling hooks without waiting for the packer");
        return;
    }

    Log::Info(L"Packed exe: holding the hooks until the engine has loaded, so the packer's check sees clean code");

    const DWORD Deadline = GetTickCount() + CheckTimeoutMs;
    while (!*Natives::GEngine)
    {
        if (GetTickCount() > Deadline)
        {
            Log::Error(L"Packed exe: no engine after {}s, enabling hooks anyway", CheckTimeoutMs / 1000);
            return;
        }
        Sleep(PollMs);
    }

    Log::Info(L"Packed exe: the engine is up, so the packer has finished checking; enabling hooks now");
}
