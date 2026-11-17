#include "pch.h"
#include "AI/Navigation/NavCollision.h"
#include "Engine/StaticMesh.h"
#include "Finder.h"
#include "Log.h"
#include "UObject/Class.h"
#include "UObject/UObjectGlobals.h"
#include "UObject/UObjectIterator.h"

namespace
{
    // `test byte ptr [reg+Offset], Mask`, with or without a REX prefix: the code reading one bool member.
    bool TestsByteMember(uintptr_t Instruction, int32_t Offset, uint8_t Mask)
    {
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Instruction);
        if (Code[0] >= 0x40 && Code[0] <= 0x4F)
        {
            ++Code;
        }
        const uint8_t ModRM = Code[1];
        if (Code[0] != 0xF6 || (ModRM & 0x38) != 0 || (ModRM & 7) == 4)
        {
            return false;
        }
        switch (ModRM >> 6)
        {
        case 1:
            return static_cast<int8_t>(Code[2]) == Offset && Code[3] == Mask;
        case 2:
            return *reinterpret_cast<const int32_t*>(Code + 2) == Offset && Code[6] == Mask;
        default:
            return false;
        }
    }

    // UNavCollision::PostInitProperties's slot (7.00 and 6.31: 8): the first of the class default's virtuals that reads
    // bCreateOnClient, which it does right after its Super call and its class default test. -2 until looked for.
    int32_t PostInitPropertiesIndex = -2;
}

UNavCollisionBase* UNavCollisionBase::ConstructNew(UObject& Outer)
{
    return UNavCollision::ConstructNew(Outer);
}

void UNavCollisionBase::Setup(UBodySetup* BodySetup)
{
    const int32_t Index = UStaticMesh::NavCollisionSetupIndex();
    if (Index >= 0 && BodySetup)
    {
        using FSetup = void (*)(UNavCollisionBase*, UBodySetup*);
        reinterpret_cast<FSetup>((*reinterpret_cast<void* const* const*>(this))[Index])(this, BodySetup);
    }
}

UNavCollisionBase* UNavCollision::ConstructNew(UObject& Outer)
{
    UClass* Class = StaticClass();
    return Class ? NewObject<UNavCollisionBase>(&Outer, Class) : nullptr;
}

void UNavCollision::PostInitProperties()
{
    if (PostInitPropertiesIndex >= 0)
    {
        using FPostInitProperties = void (*)(UNavCollision*);
        reinterpret_cast<FPostInitProperties>((*reinterpret_cast<void* const* const*>(this))[PostInitPropertiesIndex])(this);
    }
}

void UNavCollision::CreateOnServer()
{
    UClass* Class = StaticClass();
    UNavCollision* Default = Class && Member_bCreateOnClient.IsAvailable() ? static_cast<UNavCollision*>(Class->GetDefaultObject()) : nullptr;
    if (!Default)
    {
        Log::Info(L"UNavCollision: no bCreateOnClient on this build, static meshes keep the navigation collision they load with");
        return;
    }

    const bool bWasCreateOnClient = Default->bCreateOnClient;
    if (!bWasCreateOnClient)
    {
        if (PostInitPropertiesIndex == -2)
        {
            PostInitPropertiesIndex = -1;
            const int32_t Offset = Member_bCreateOnClient.GetOffsetChecked();
            const uint8_t Mask = Member_bCreateOnClient.GetFieldMask();
            void* const* VTable = *reinterpret_cast<void* const* const*>(Default);
            for (int32_t Slot = 0; Slot < 24 && PostInitPropertiesIndex < 0; ++Slot)
            {
                const uintptr_t Function = reinterpret_cast<uintptr_t>(VTable[Slot]);
                if (Function && Finder::TraceFind(Function, 24, [&](uintptr_t Instruction) { return TestsByteMember(Instruction, Offset, Mask); }))
                {
                    PostInitPropertiesIndex = Slot;
                    Log::Info(L"Finder: UNavCollision::PostInitProperties is slot 0x{:X} (+0x{:X})", Slot, Function - Finder::ImageBase());
                }
            }
        }
        if (PostInitPropertiesIndex < 0)
        {
            Log::Error(L"Finder: UNavCollision::PostInitProperties not found (the virtual reading bCreateOnClient): static meshes get no navigation collision, so Save the World's navmeshes have nothing to build on");
            return;
        }
        Default->bCreateOnClient = true;
        Default->PostInitProperties();
    }

    int32_t Meshes = 0;
    int32_t Had = 0;
    int32_t Created = 0;
    for (TObjectIterator<UStaticMesh> It; It; ++It)
    {
        UStaticMesh* Mesh = *It;
        if (Mesh->HasAnyFlags(RF_ClassDefaultObject | RF_ArchetypeObject))
        {
            continue;
        }
        ++Meshes;
        if (Mesh->NavCollision)
        {
            ++Had;
            continue;
        }
        Mesh->CreateNavCollision();
        Created += Mesh->NavCollision ? 1 : 0;
    }
    Log::Info(L"UNavCollision: static meshes get their navigation collision on this server (bCreateOnClient {}); of the {} loaded, {} had one, {} got one now",
        bWasCreateOnClient ? L"was on" : L"turned on and the class default's PostInitProperties run again", Meshes, Had, Created);
}
