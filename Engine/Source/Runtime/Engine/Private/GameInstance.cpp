#include "pch.h"
#include "Engine/GameInstance.h"
#include "CoreGlobals.h"
#include "Engine/Engine.h"
#include "HAL/UnrealMemory.h"
#include "Hooks.h"
#include "Natives.h"

#include <cstring>

bool UGameInstance::IsDedicatedServerInstance() const
{
    if (IsRunningDedicatedServer())
    {
        return true;
    }

    return WorldContext ? WorldContext->RunAsDedicated : false;
}

TSharedPtr<const FUniqueNetId> UGameInstance::GetGameSessionId() const
{
    TSharedPtr<const FUniqueNetId> SessionId;
    if (!Natives::UGameInstance_GetGameSessionId)
    {
        return SessionId;
    }

    // The native fills an out value of one of two types. Older builds' is a TSharedPtr<const FUniqueNetId> (object,
    // reference controller). 4.4.2's (0xC534E0) is an FUniqueNetIdRepl: its vtable (0x3233D60), that TSharedPtr, then
    // ReplicationBytes, 0x28 bytes, as RemoveParticipantAccount (0xDC6460) destroys it. Read as a bare TSharedPtr, the
    // vtable became the id, and a lost Save the World mission crashed in OnZoneEnd on the id's IsValid. The out value
    // gets room for either. An FUniqueNetIdRepl starts with its vtable, inside the exe's image; a bare TSharedPtr starts
    // with the id, which is always heap-allocated (MakeShareable/MakeShared).
    alignas(16) uint8_t Result[0x40] = {};
    Natives::UGameInstance_GetGameSessionId(const_cast<UGameInstance*>(this), Result);

    const auto ImageBase = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* NtHeaders = reinterpret_cast<const IMAGE_NT_HEADERS*>(ImageBase + reinterpret_cast<const IMAGE_DOS_HEADER*>(ImageBase)->e_lfanew);
    const uintptr_t First = *reinterpret_cast<const uintptr_t*>(Result);
    const bool bIsNetIdRepl = First >= ImageBase && First < ImageBase + NtHeaders->OptionalHeader.SizeOfImage;

    // The TSharedPtr is moved out bitwise, so its reference is now SessionId's to release.
    std::memcpy(static_cast<void*>(&SessionId), Result + (bIsNetIdRepl ? 0x8 : 0x0), sizeof(SessionId));
    if (bIsNetIdRepl)
    {
        // FUniqueNetIdRepl::ReplicationBytes (TArray<uint8>): GetGameSessionId leaves it empty, but free it if set.
        if (void* ReplicationBytes = *reinterpret_cast<void**>(Result + 0x18))
        {
            FMemory::Free(ReplicationBytes);
        }
    }
    return SessionId;
}

void UGameInstance::Hook()
{
    Hooks::Detour(L"UGameInstance::IsDedicatedServerInstance", reinterpret_cast<void*>(Natives::UGameInstance_IsDedicatedServerInstance), &IsDedicatedServerInstanceHK, nullptr);
}

bool UGameInstance::IsDedicatedServerInstanceHK(const UGameInstance* This)
{
    return This->IsDedicatedServerInstance();
}
