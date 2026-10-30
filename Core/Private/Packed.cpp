#include "pch.h"
#include "Packed.h"
#include "Log.h"
#include "Natives.h"
#include "ObjectLayout.h"
#include "Finder.h"
#include "Hooks.h"
#include "UObject/NameTypes.h"
#include "UObject/UObjectArray.h"
#include "UObject/UObjectBase.h"

#include <atomic>

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

namespace
{
    constexpr DWORD EngineTimeoutMs = 300000;

    using FEngineInit = void (*)(void* Engine, void* EngineLoop);

    // The engine classes whose Init is held: each class's vtable and the Init it had. An entry is complete before the
    // slot is swapped, and only ever added to, so the engine thread reads it without a lock.
    struct FHeldInit
    {
        void** VTable = nullptr;
        FEngineInit Original = nullptr;
    };
    constexpr int32_t MaxHeldInits = 32;
    FHeldInit HeldInits[MaxHeldInits];
    std::atomic<int32_t> HeldInitCount = 0;

    void (*GBeforeEngineInit)() = nullptr;
    std::atomic<bool> bBeforeEngineInitRan = false;

    void RunBeforeEngineInitOnce(const wchar_t* Where)
    {
        if (!bBeforeEngineInitRan.exchange(true))
        {
            Log::Info(L"Packed exe: {}", Where);
            GBeforeEngineInit();
        }
    }

    const FHeldInit* FindHeld(void** VTable)
    {
        for (int32_t Index = 0; Index < HeldInitCount.load(); ++Index)
        {
            if (HeldInits[Index].VTable == VTable)
            {
                return &HeldInits[Index];
            }
        }
        return nullptr;
    }

    // UEngine::Init, entered from FEngineLoop::Init on the engine's own thread just after it set GEngine: the initial
    // load the packer checks at the end of is over, and nothing of the engine's own start-up has run yet.
    void EngineInitHK(void* Engine, void* EngineLoop)
    {
        RunBeforeEngineInitOnce(L"the engine is about to initialise and the packer has checked its code; Core takes over here");

        const FHeldInit* Held = FindHeld(*reinterpret_cast<void***>(Engine));
        if (!Held)
        {
            Log::Fatal(L"Packed exe: UEngine::Init reached through a vtable Core didn't hold");
            return;
        }
        Held->Original(Engine, EngineLoop);
    }

    const UObjectBase* ClassOf(const UObjectBase* Object)
    {
        return reinterpret_cast<const UObjectBase*>(Object->GetClass());
    }

    const UObjectBase* SuperOf(const UObjectBase* Class)
    {
        return *reinterpret_cast<const UObjectBase* const*>(reinterpret_cast<uintptr_t>(Class) + ObjectLayout::Current.UStruct_SuperStruct);
    }

    // Visits every class default object there is so far. The engine makes the default objects of its native classes
    // as their modules load, before FEngineLoop::Init creates GEngine from one of those classes. GUObjectArray is a
    // fixed block, so reading it while the engine adds to it is safe.
    template <typename FVisit>
    void ForEachDefaultObject(FVisit&& Visit)
    {
        const FUObjectArray* Array = Natives::GUObjectArray;
        for (int32_t Index = 0; Array && Index < Array->GetObjectArrayNum(); ++Index)
        {
            const FUObjectItem* Item = Array->IndexToObject(Index);
            UObjectBase* Object = Item ? Item->Object : nullptr;
            if (Object && (Object->GetFlags() & RF_ClassDefaultObject) && Object->GetClass())
            {
                Visit(Object);
            }
        }
    }

    // The class of the default object called Name, once the engine has made it.
    const UObjectBase* ClassOfDefaultObject(const wchar_t* Name)
    {
        const FName Wanted(Name, FNAME_Find);
        const UObjectBase* Found = nullptr;
        if (!Wanted.IsNone())
        {
            ForEachDefaultObject([&](UObjectBase* Object)
            {
                Found = !Found && Object->GetFName() == Wanted ? ClassOf(Object) : Found;
            });
        }
        return Found;
    }

    // Holds Init in the vtable of every class there is so far that is Base or derives from it.
    void HoldInits(const UObjectBase* Base)
    {
        ForEachDefaultObject([&](UObjectBase* Object)
        {
            bool bDerived = false;
            int32_t Depth = 0;
            for (const UObjectBase* Class = ClassOf(Object); Class && Depth < 64 && !bDerived; Class = SuperOf(Class), ++Depth)
            {
                bDerived = Class == Base;
            }
            void** const VTable = *reinterpret_cast<void***>(Object);
            const int32_t Count = HeldInitCount.load();
            if (!bDerived || FindHeld(VTable) || Count >= MaxHeldInits)
            {
                return;
            }

            HeldInits[Count] = { VTable, reinterpret_cast<FEngineInit>(VTable[Natives::UEngine_InitIndex]) };
            HeldInitCount.store(Count + 1);
            const std::wstring Name = L"UEngine::Init (" + ClassOf(Object)->GetFName().ToStdString() + L")";
            Hooks::SwapVTable(Name, VTable, Natives::UEngine_InitIndex, reinterpret_cast<void*>(&EngineInitHK));
        });
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

bool Packed::RunBeforeEngineInit(const wchar_t* EngineDefaultObject, void (*Callback)())
{
    if (!Natives::GEngine || !Natives::GUObjectArray || Natives::UEngine_InitIndex < 0)
    {
        Log::Error(L"Packed exe: GEngine, GUObjectArray or UEngine::Init's slot is missing, so Core can't stop the engine before it initialises");
        return false;
    }
    GBeforeEngineInit = Callback;

    // The engine's class and so its vtable exist before the engine does: GEngine is made from the class the config
    // names (Fortnite's FortEngine), a subclass of the one named here. Its Init slot is data, not code, so swapping it
    // leaves the code the packer checks as it is, and holds the engine at a fixed point after that check: Init is
    // FEngineLoop::Init's next virtual call on the engine after it sets GEngine (Finder::ResolveEngineInit). Every
    // subclass is held, since which one the config names isn't known here, and the scan repeats until the engine
    // exists, as a module loaded later can add one.
    const UObjectBase* Base = nullptr;
    const DWORD Deadline = GetTickCount() + EngineTimeoutMs;
    while (!(Base = ClassOfDefaultObject(EngineDefaultObject)))
    {
        if (*Natives::GEngine || GetTickCount() > Deadline)
        {
            Log::Error(L"Packed exe: {} not found before the engine was created", EngineDefaultObject);
            return false;
        }
        Sleep(1);
    }

    HoldInits(Base);
    if (*Natives::GEngine)
    {
        Log::Error(L"Packed exe: the engine was created before Core could hold its Init");
        return false;
    }
    Log::Info(L"Packed exe: holding the engine at UEngine::Init until the packer has checked its code ({} engine classes so far)", HeldInitCount.load());

    while (!*Natives::GEngine)
    {
        if (GetTickCount() > Deadline)
        {
            Log::Error(L"Packed exe: no engine after {}s", EngineTimeoutMs / 1000);
            return true;
        }
        HoldInits(Base);
        Sleep(PollMs);
    }

    // Should the engine be of a class that appeared between the last scan and its creation, fall back to running as
    // soon as the engine exists, from this thread, which races the engine's Init but beats leaving the server a client.
    const UObjectBase* Engine = reinterpret_cast<const UObjectBase*>(*Natives::GEngine);
    const std::wstring EngineClass = ClassOf(Engine)->GetFName().ToStdString();
    if (!FindHeld(*reinterpret_cast<void** const*>(Engine)))
    {
        Log::Error(L"Packed exe: the engine is a {}, whose Init Core didn't hold, so it initialises without waiting for Core", EngineClass);
        RunBeforeEngineInitOnce(L"the engine is up, so the packer has checked its code; Core takes over late");
        return true;
    }
    Log::Info(L"Packed exe: the engine is a {}, held at its Init", EngineClass);
    return true;
}
