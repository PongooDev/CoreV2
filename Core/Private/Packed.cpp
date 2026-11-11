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
#include "Engine/World.h"

#include <atomic>

namespace
{
    /// <summary>The opening of an x64 function with a frame, `mov [rsp+8], rbx`. Decrypted game code is full of
    /// them and ciphertext has them only by chance, which is what tells the two apart.</summary>
    constexpr uint8_t Prologue[] = { 0x48, 0x89, 0x5C, 0x24 };
    // Counted over all the samples. The packed image has a few by chance or left in the clear (2.4.2's four windows
    // hold 17 before the stub runs, 2.5's 16), a decrypted one hundreds (2.5's 362, with one window in its thunks).
    constexpr int PrologueThreshold = 64;
    constexpr size_t SampleSize = 0x10000;
    constexpr int SampleCount = 4;

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
    // How long to let a 3.5+ menu tick before patching, so the packer's recheck runs on clean code while Core still
    // patches as early as it can: measured on 3.5.1, 1s still died (and left a null call at frame 1) and 3s was clean
    // over repeated runs, so 4s keeps a margin. Hooks then land while the menu is still on its login flow.
    constexpr DWORD TickSettleMs = 4000;

    // 3.5 (CL-4000805) is the first build whose packer rechecks the code on the first tick after the menu map loads; up
    // to 3.3 (CL-3942182) it only checks once as the load ends, so those patch at UEngine::Init / the engine coming up
    // as they always have. The extra ticking wait is only for this changelist on, where frame-0 patches crash.
    constexpr uint32_t FirstRecheckChangelist = 4000805;

    // The build's changelist from its version string in .rdata ("++Fortnite+Release-3.5-CL-4000805"); 0 when missing.

    uint32_t BuildChangelist()
    {
        static const uint32_t Changelist = []
        {
            // The first copy names the build ("++Fortnite+Release-4.5-CL-4166199") up to 4.5; 5.0 has the bare branch
            // name ("++Fortnite+Release-5.0") first, so the first copy with a changelist counts.
            static constexpr wchar_t Prefix[] = L"++Fortnite+";
            for (uintptr_t Found = Finder::FindReadOnly(Prefix, sizeof(Prefix) - sizeof(wchar_t)); Found; Found = Finder::FindReadOnlyAfter(Prefix, sizeof(Prefix) - sizeof(wchar_t), Found))
            {
                std::wstring Version;
                for (const wchar_t* Char = reinterpret_cast<const wchar_t*>(Found); Version.size() < 128 && Finder::IsReadable(Char, sizeof(wchar_t)) && *Char; ++Char)
                {
                    Version.push_back(*Char);
                }
                const size_t At = Version.find(L"-CL-");
                if (At != std::wstring::npos)
                {
                    return static_cast<uint32_t>(std::wcstoul(Version.c_str() + At + 4, nullptr, 10));
                }
            }
            return 0u;
        }();
        return Changelist;
    }

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

        // Spread the samples over .text, counting across all of them. Its first pages are reached before the rest and
        // a sample there would say "decrypted" early, and a single one further in is not enough either: a stretch of
        // the generated registration thunks that 2.5 has at .text+0x100000, where the one sample used to go, is all
        // `mov r9d, hash / lea rdx, name / lea rcx, slot / jmp` and holds no prologue at all.
        const size_t Length = Image.TextSize < SampleSize ? Image.TextSize : SampleSize;
        const size_t Room = Image.TextSize - Length;

        int Count = 0;
        for (int Sample = 1; Sample <= SampleCount; ++Sample)
        {
            const uint8_t* Bytes = Image.Text + Room * Sample / (SampleCount + 1);
            for (size_t At = 0; At + sizeof(Prologue) <= Length; ++At)
            {
                if (memcmp(Bytes + At, Prologue, sizeof(Prologue)) == 0 && ++Count >= PrologueThreshold)
                {
                    return true;
                }
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

    // The default object called Name, once the engine has made it.
    const UObjectBase* FindDefaultObject(const wchar_t* Name)
    {
        const FName Wanted(Name, FNAME_Find);
        const UObjectBase* Found = nullptr;
        if (!Wanted.IsNone())
        {
            ForEachDefaultObject([&](UObjectBase* Object)
            {
                Found = !Found && Object->GetFName() == Wanted ? Object : Found;
            });
        }
        return Found;
    }

    // The class of the default object called Name, once the engine has made it.
    const UObjectBase* ClassOfDefaultObject(const wchar_t* Name)
    {
        const UObjectBase* Found = FindDefaultObject(Name);
        return Found ? ClassOf(Found) : nullptr;
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
    if (Image.TextSections < 2 || (HandedOver(Image) && LooksDecrypted(Image)))
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

bool Packed::ChecksCodeOnFirstTicks()
{
    return IsPacked() && BuildChangelist() >= FirstRecheckChangelist;
}

uint32_t Packed::Changelist()
{
    return BuildChangelist();
}

void Packed::WaitForCodeCheck(bool bUntilTicking)
{
    if (Describe().TextSections < 2)
    {
        return;
    }

    // GEngine is set in FEngineLoop::Init, the step after the initial load whose end the packer checks at, so the
    // engine existing means that first check has been and gone. Waiting on a timer instead would be guesswork: the
    // window is a phase of start-up, and how long the phase takes moves around between runs.
    if (!Natives::GEngine)
    {
        Log::Error(L"Packed exe: GEngine was never resolved, so enabling hooks without waiting for the packer");
        return;
    }

    Log::Info(L"Packed exe: holding the hooks until the engine has loaded, so the packer's check sees clean code");

    // No deadline that patches anyway: on a slow or busy PC the initial load has taken over 5 minutes (6.00 with
    // several servers running: GetGameData alone took 370 s), and patching before the packer's check crashes the
    // process. A load that never ends hangs either way, so the wait only says, once, that it is still waiting.
    const DWORD Deadline = GetTickCount() + CheckTimeoutMs;
    bool bReportedSlow = false;
    while (!*Natives::GEngine)
    {
        if (!bReportedSlow && GetTickCount() > Deadline)
        {
            bReportedSlow = true;
            Log::Warning(L"Packed exe: no engine after {}s; the load is slow, still holding the hooks until it is up", CheckTimeoutMs / 1000);
        }
        Sleep(PollMs);
    }

    // 3.5 checks the code again on the first tick after its menu map loads, so on those builds the engine merely being
    // up is too early. Wait on past it: until a world exists (the menu map is loading) and then a settle margin, which
    // puts that tick's check well before any patch. The margin only slows a packed client/launcher's menu by a few
    // seconds. The server doesn't wait here: it holds the engine at UEngine::Start instead (RunBeforeEngineStart).
    if (bUntilTicking && Natives::GWorld && BuildChangelist() >= FirstRecheckChangelist)
    {
        while (!*Natives::GWorld)
        {
            if (!bReportedSlow && GetTickCount() > Deadline)
            {
                bReportedSlow = true;
                Log::Warning(L"Packed exe: no world after {}s; the load is slow, still holding the hooks until it is up", CheckTimeoutMs / 1000);
            }
            Sleep(PollMs);
        }
        const DWORD Settle = TickSettleMs;
        Log::Info(L"Packed exe: a world is up; letting it tick {}ms past the menu-load check before patching", Settle);
        Sleep(Settle);
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

namespace
{
    using FEngineStart = void (*)(void* Engine);
    using FEngineTick = void (*)(void* Engine, float DeltaSeconds, bool bIdleMode);

    // UEngine's virtuals go Init, Start, PreExit, ReleaseAudioDeviceManager, Tick (Engine.h); 3.5.1's are 0x4A to 0x4E.
    constexpr int32_t StartAfterInit = 1;
    constexpr int32_t TickAfterInit = 4;

    // Set before the slots are swapped, and only used on the engine's thread after.
    FEngineStart GHeldStart = nullptr;
    FEngineTick GHeldTick = nullptr;
    void (*GBeforeEngineStart)() = nullptr;
    bool bStartHeld = false;
    bool bStarted = false;
    uint32_t HeldFrames = 0;
    DWORD FirstHeldFrame = 0;

    // UEngine::Start, FEngineLoop::Init's last call on the engine before its main loop: UGameEngine::Start has the game
    // instance browse to the default map. Held, the engine goes into its loop with the empty world
    // UGameInstance::InitializeStandalone made, and nothing of the server's has loaded, listened or registered yet.
    void EngineStartHK(void* Engine)
    {
        bStartHeld = true;
        Log::Info(L"Packed exe: holding the engine at UEngine::Start, so its first ticks run on unpatched code for the packer's check");
    }

    // UEngine::Tick, called by FEngineLoop::Tick every frame. While Start is held it counts the frames the engine runs
    // without a map, and once they are past the packer's check (and a settle margin, as the client has) runs Core's
    // callback and then the held Start at the top of the frame: where the engine's own travel loads a map
    // (UGameEngine::Tick's TickWorldTravel), so the frame goes on to tick the world Start loaded.
    void EngineTickHK(void* Engine, float DeltaSeconds, bool bIdleMode)
    {
        if (bStartHeld && !bStarted)
        {
            const DWORD Now = GetTickCount();
            if (HeldFrames++ == 0)
            {
                FirstHeldFrame = Now;
                Log::Info(L"Packed exe: the engine is ticking without a map; letting it run {}ms past the packer's check before patching", TickSettleMs);
            }
            if (HeldFrames > 1 && Now - FirstHeldFrame >= TickSettleMs)
            {
                bStarted = true;
                Log::Info(L"Packed exe: {} frames past the packer's check; Core patches the code and starts the engine", HeldFrames);
                GBeforeEngineStart();
                GHeldStart(Engine);
            }
        }
        GHeldTick(Engine, DeltaSeconds, bIdleMode);
    }
}

bool Packed::RunBeforeEngineStart(void (*Callback)())
{
    UObjectBase* Engine = Natives::GEngine ? reinterpret_cast<UObjectBase*>(*Natives::GEngine) : nullptr;
    const UObjectBase* GameEngine = FindDefaultObject(L"Default__GameEngine");
    if (!Engine || !GameEngine || Natives::UEngine_InitIndex < 0 || !Natives::UGameEngine_Tick)
    {
        Log::Error(L"Packed exe: the engine, GameEngine's default object, UEngine::Init's slot or UGameEngine::Tick is missing, so Core can't hold the engine at UEngine::Start");
        return false;
    }

    // The slots, checked against UGameEngine's own vtable: its Start logs "Starting Game." and its Tick is the one the
    // finders resolved. The engine's own class (FortEngine) overrides both and calls these.
    void** const GameEngineVTable = *reinterpret_cast<void** const*>(GameEngine);
    const int32_t StartIndex = Natives::UEngine_InitIndex + StartAfterInit;
    const int32_t TickIndex = Natives::UEngine_InitIndex + TickAfterInit;
    uintptr_t GameEngineStart = 0;
    for (const uintptr_t Reference : Finder::StringRefs(L"Starting Game."))
    {
        GameEngineStart = GameEngineStart ? GameEngineStart : Finder::FunctionStart(Reference);
    }
    if (!GameEngineStart || reinterpret_cast<uintptr_t>(GameEngineVTable[StartIndex]) != GameEngineStart
        || GameEngineVTable[TickIndex] != reinterpret_cast<void*>(Natives::UGameEngine_Tick))
    {
        Log::Error(L"Packed exe: UGameEngine's vtable has no Start at slot 0x{:X} or no Tick at slot 0x{:X}, so Core can't hold the engine at UEngine::Start", StartIndex, TickIndex);
        return false;
    }

    void** const VTable = *reinterpret_cast<void** const*>(Engine);
    GBeforeEngineStart = Callback;
    const std::wstring EngineClass = ClassOf(Engine)->GetFName().ToStdString();
    return Hooks::SwapVTable(L"UEngine::Tick (" + EngineClass + L")", VTable, TickIndex, reinterpret_cast<void*>(&EngineTickHK), reinterpret_cast<void**>(&GHeldTick))
        && Hooks::SwapVTable(L"UEngine::Start (" + EngineClass + L")", VTable, StartIndex, reinterpret_cast<void*>(&EngineStartHK), reinterpret_cast<void**>(&GHeldStart));
}
