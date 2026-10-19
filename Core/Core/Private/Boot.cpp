#include "pch.h"
#include "Boot.h"
#include "Configuration.h"
#include "Finder.h"
#include "GameThread.h"
#include "Hooks.h"
#include "Log.h"
#include "Natives.h"
#include "Reflection.h"
#include "Containers/UnrealString.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>

namespace
{
    constexpr int32_t FNetworkNotifyOffset = 0x28;
    constexpr int32_t NAME_GameNetDriver = 282;

    void (*OriginalStartGameInstance)(void*) = nullptr;
    void (*OriginalTick)(void*, float, bool) = nullptr;

    std::atomic<bool> LiveResolved = false;
    std::atomic<bool> ReadyLogged = false;

    int32_t OffsetOf(UStruct* Owner, const wchar_t* Name)
    {
        UProperty* Property = Owner ? Owner->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
        return Property ? Property->GetOffset_ForInternal() : -1;
    }

    template <typename T>
    T& MemberAt(void* Base, int32_t Offset)
    {
        return *reinterpret_cast<T*>(static_cast<uint8_t*>(Base) + Offset);
    }

    void CallInitListen(void* NetDriver, void* Notify, void* URL, FString& Error)
    {
        using FInitListen = bool (*)(void*, void*, void*, bool, FString&);
        void** VTable = *reinterpret_cast<void***>(NetDriver);
        reinterpret_cast<FInitListen>(VTable[Natives::UNetDriver_InitListenIndex])(NetDriver, Notify, URL, false, Error);
    }

    bool CoreListen(void* World, void* URL)
    {
        UObject* WorldObject = static_cast<UObject*>(World);
        UStruct* WorldClass = WorldObject->GetClass();

        const int32_t NetDriverOffset = OffsetOf(WorldClass, L"NetDriver");
        const int32_t LevelCollectionsOffset = OffsetOf(WorldClass, L"LevelCollections");
        if (NetDriverOffset < 0 || LevelCollectionsOffset < 0 || Natives::UNetDriver_InitListenIndex < 0)
        {
            Log::Error(L"Listen: required layout is not resolved");
            return false;
        }

        void*& NetDriver = MemberAt<void*>(World, NetDriverOffset);
        if (NetDriver)
        {
            Log::Warning(L"Listen: a net driver already exists");
            return false;
        }

        FName DriverName(L"GameNetDriver", FNAME_Find);
        if (Natives::UEngine_CreateNamedNetDriver(*Natives::GEngine, static_cast<UWorld*>(World), DriverName, DriverName))
        {
            NetDriver = Natives::UEngine_FindNamedNetDriver(*Natives::GEngine, static_cast<UWorld*>(World), DriverName);
            if (NetDriver)
            {
                Natives::UNetDriver_SetWorld(static_cast<UNetDriver*>(NetDriver), static_cast<UWorld*>(World));

                UScriptStruct* Collection = FindObject<UScriptStruct>(nullptr, L"/Script/Engine.LevelCollection");
                const int32_t CollectionNetDriverOffset = OffsetOf(Collection, L"NetDriver");
                const int32_t CollectionSize = Collection ? Collection->GetPropertiesSize() : 0;
                void* Collections = MemberAt<void*>(World, LevelCollectionsOffset);
                const int32_t CollectionCount = MemberAt<int32_t>(World, LevelCollectionsOffset + sizeof(void*));
                if (Collections && CollectionNetDriverOffset >= 0 && CollectionSize > 0)
                {
                    for (int32_t Index = 0; Index < CollectionCount; ++Index)
                    {
                        void* Entry = static_cast<uint8_t*>(Collections) + Index * CollectionSize;
                        const int32_t Type = *reinterpret_cast<int32_t*>(Entry);
                        if (Type == 0 || Type == 2)
                        {
                            MemberAt<void*>(Entry, CollectionNetDriverOffset) = NetDriver;
                        }
                    }
                }
            }
        }

        if (!NetDriver)
        {
            Log::Error(L"Listen: failed to create the game net driver");
            return false;
        }

        FString Error;
        CallInitListen(NetDriver, static_cast<uint8_t*>(World) + FNetworkNotifyOffset, URL, Error);
        if (!Error.IsEmpty())
        {
            Log::Error(L"Listen: InitListen failed: {}", Error.ToStdString());
            Natives::UNetDriver_SetWorld(static_cast<UNetDriver*>(NetDriver), nullptr);
            NetDriver = nullptr;
            return false;
        }

        UStruct* DriverClass = static_cast<UObject*>(NetDriver)->GetClass();
        const int32_t MaxClientRate = OffsetOf(DriverClass, L"MaxClientRate");
        const int32_t MaxInternetClientRate = OffsetOf(DriverClass, L"MaxInternetClientRate");
        if (MaxClientRate >= 0 && MaxInternetClientRate >= 0)
        {
            const int32_t Internet = MemberAt<int32_t>(NetDriver, MaxInternetClientRate);
            if (Internet < MemberAt<int32_t>(NetDriver, MaxClientRate) && Internet > 2500)
            {
                MemberAt<int32_t>(NetDriver, MaxClientRate) = Internet;
            }
        }

        Log::Info(L"Listen: game net driver is listening");
        return true;
    }

    uint8_t Byte(uintptr_t Address)
    {
        return *reinterpret_cast<const uint8_t*>(Address);
    }

    bool PatchPreInitServerMode()
    {
        uint8_t* Site = Sites::PreInitRegularClient;
        if (!Site)
        {
            return false;
        }

        const uint8_t RegularFlag[] = { 0x41, 0xB5, 0x00 };
        const uint8_t ServerStore[] = { 0xC6, 0x05, Site[6], Site[7], Site[8], Site[9], 0x01 };
        return Hooks::WriteBytes(L"PreInit GIsRegularClient", Site, RegularFlag)
            && Hooks::WriteBytes(L"PreInit GIsServer", Site + 3, ServerStore);
    }

    bool PatchInlinedNetMode()
    {
        if (!Natives::UWorld_AttemptDeriveFromURL)
        {
            return false;
        }

        const std::vector<uintptr_t> Sites = Finder::CallsTo(reinterpret_cast<uintptr_t>(Natives::UWorld_AttemptDeriveFromURL), true);
        int32_t Patched = 0;
        for (uintptr_t Site : Sites)
        {
            uintptr_t DemoLoad = 0;
            for (uintptr_t Scan = Site - 0x40; Scan + 7 <= Site; ++Scan)
            {
                const uint8_t Rex = Byte(Scan);
                if ((Rex & 0xF8) == 0x48 && Byte(Scan + 1) == 0x8B && (Byte(Scan + 2) & 0xC0) == 0x80 && Byte(Scan + 3) == 0xB8 && Byte(Scan + 4) == 0x00 && Byte(Scan + 5) == 0x00 && Byte(Scan + 6) == 0x00)
                {
                    DemoLoad = Scan;
                }
            }
            if (!DemoLoad)
            {
                continue;
            }

            uintptr_t Jcc = 0;
            for (uintptr_t Scan = DemoLoad - 0x18; Scan < DemoLoad; ++Scan)
            {
                const uint8_t Rex = Byte(Scan);
                if ((Rex & 0xF8) != 0x48)
                {
                    continue;
                }
                if (Byte(Scan + 1) == 0x83 && (Byte(Scan + 2) & 0xC0) == 0x40 && (Byte(Scan + 2) & 0x38) == 0x38 && Byte(Scan + 3) == 0x38 && Byte(Scan + 4) == 0x00)
                {
                    Jcc = Scan + 5;
                }
                else if (Byte(Scan + 1) == 0x39 && (Byte(Scan + 2) & 0xC0) == 0x40 && Byte(Scan + 3) == 0x38)
                {
                    Jcc = Scan + 4;
                }
            }
            if (!Jcc || (Byte(Jcc) != 0x74 && Byte(Jcc) != 0x75))
            {
                continue;
            }

            const uint8_t JccPatch[] = { Byte(Jcc) == 0x74 ? static_cast<uint8_t>(0xEB) : static_cast<uint8_t>(0x90), Byte(Jcc) == 0x74 ? Byte(Jcc + 1) : static_cast<uint8_t>(0x90) };
            const uint8_t DemoPatch[] = { 0x38 };
            if (Hooks::WriteBytes(L"Inlined net mode jcc", reinterpret_cast<void*>(Jcc), JccPatch)
                && Hooks::WriteBytes(L"Inlined net mode driver load", reinterpret_cast<void*>(DemoLoad + 3), DemoPatch))
            {
                ++Patched;
            }
        }

        Log::Info(L"Net mode: patched {} of {} inlined copies", Patched, Sites.size());
        return Patched == static_cast<int32_t>(Sites.size()) && Patched > 0;
    }

    void TickHook(void* This, float DeltaSeconds, bool bIdleMode)
    {
        GameThread::Tick();

        if (!ReadyLogged.exchange(true))
        {
            Log::Info(L"Server ready");
        }

        OriginalTick(This, DeltaSeconds, bIdleMode);
    }

    void StartGameInstanceHook(void* This)
    {
        if (!LiveResolved.exchange(true))
        {
            Finder::ResolveInitListen();

            if (Sites::LoadMapListenCall)
            {
                Hooks::RedirectCall(L"UWorld::Listen", Sites::LoadMapListenCall, reinterpret_cast<void*>(&CoreListen));
            }
        }

        OriginalStartGameInstance(This);
    }
}

void Boot::Initialize()
{
    Configuration::Load();
    Log::Initialize(Configuration::Get().bConsole);
    Log::Info(L"Core V2 starting");

    if (MH_Initialize() != MH_OK)
    {
        Log::Error(L"MinHook failed to initialize");
        return;
    }

    if (!Finder::ResolveAll())
    {
        Log::Error(L"Startup aborted: required addresses are missing");
        return;
    }

    PatchPreInitServerMode();
    PatchInlinedNetMode();

    Hooks::Detour(L"UGameInstance::StartGameInstance", reinterpret_cast<void*>(Natives::UGameInstance_StartGameInstance), StartGameInstanceHook, &OriginalStartGameInstance);
    Hooks::Detour(L"UGameEngine::Tick", reinterpret_cast<void*>(Natives::UGameEngine_Tick), TickHook, &OriginalTick);
    Hooks::EnableAll();

    Log::Info(L"Core V2 startup complete");
}
