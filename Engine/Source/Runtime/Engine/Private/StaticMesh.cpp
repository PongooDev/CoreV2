#include "pch.h"
#include "Engine/StaticMesh.h"
#include "AI/Navigation/NavCollision.h"
#include "Finder.h"
#include "Log.h"
#include "UObject/Class.h"

namespace
{
    // `test byte ptr [reg+Offset], Mask`, with or without a REX prefix.
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

    // `mov [reg+Offset], r64`: a pointer member stored.
    bool StoresPointerMember(uintptr_t Instruction, int32_t Offset)
    {
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Instruction);
        if ((Code[0] & 0xF8) != 0x48 || Code[1] != 0x89 || (Code[2] & 7) == 4)
        {
            return false;
        }
        switch (Code[2] >> 6)
        {
        case 1:
            return static_cast<int8_t>(Code[3]) == Offset;
        case 2:
            return *reinterpret_cast<const int32_t*>(Code + 3) == Offset;
        default:
            return false;
        }
    }

    // `call/jmp qword ptr [reg+Displacement]`: the displacement of a virtual call, or -1.
    int32_t VirtualCallDisplacement(uintptr_t Instruction)
    {
        const uint8_t* Code = reinterpret_cast<const uint8_t*>(Instruction);
        if (Code[0] >= 0x40 && Code[0] <= 0x4F)
        {
            ++Code;
        }
        const uint8_t ModRM = Code[1];
        const uint8_t Operation = (ModRM >> 3) & 7;
        if (Code[0] != 0xFF || (Operation != 2 && Operation != 4) || (ModRM & 7) == 4)
        {
            return -1;
        }
        switch (ModRM >> 6)
        {
        case 1:
            return static_cast<int8_t>(Code[2]);
        case 2:
            return *reinterpret_cast<const int32_t*>(Code + 2);
        default:
            return -1;
        }
    }
}

void UStaticMesh::CreateNavCollision([[maybe_unused]] const bool bIsUpdate)
{
    // bIsUpdate only invalidates the old collision in the editor.
    if (bHasNavigationData && BodySetup)
    {
        if (!NavCollision)
        {
            NavCollision = UNavCollisionBase::ConstructNew(*this);
        }
        if (UNavCollisionBase* Collision = NavCollision)
        {
            Collision->Setup(BodySetup);
        }
    }
}

int32_t UStaticMesh::NavCollisionSetupIndex()
{
    static const int32_t Index = []
    {
        UClass* Class = StaticClass();
        UObject* Default = Class && Member_bHasNavigationData.IsAvailable() && Member_NavCollision.IsAvailable() ? Class->GetDefaultObject() : nullptr;
        void* const* VTable = Default ? *reinterpret_cast<void* const* const*>(Default) : nullptr;
        const int32_t HasNavigationDataOffset = VTable ? Member_bHasNavigationData.GetOffsetChecked() : 0;
        const uint8_t HasNavigationDataMask = Member_bHasNavigationData.GetFieldMask();
        const int32_t NavCollisionOffset = VTable ? Member_NavCollision.GetOffsetChecked() : 0;
        // PostLoad: the slot whose code tests bHasNavigationData, then stores the collision ConstructNew made and calls
        // Setup on it (7.00 slot 0xE, 0x2A742E0).
        for (int32_t Slot = 0; VTable && Slot < 48; ++Slot)
        {
            const uintptr_t Function = reinterpret_cast<uintptr_t>(VTable[Slot]);
            const uintptr_t End = Function ? Finder::FunctionEnd(Function) : 0;
            for (uintptr_t Address = Function; End && Address + 7 <= End; ++Address)
            {
                if (!TestsByteMember(Address, HasNavigationDataOffset, HasNavigationDataMask))
                {
                    continue;
                }
                const uintptr_t Store = Finder::TraceFind(Address, 24, [&](uintptr_t Instruction) { return StoresPointerMember(Instruction, NavCollisionOffset); });
                const uintptr_t Call = Store ? Finder::TraceFind(Store, 16, [](uintptr_t Instruction) { return VirtualCallDisplacement(Instruction) > 0; }) : 0;
                const int32_t Displacement = Call ? VirtualCallDisplacement(Call) : -1;
                if (Displacement > 0 && Displacement % 8 == 0)
                {
                    Log::Info(L"Finder: UNavCollisionBase::Setup is slot 0x{:X} (UStaticMesh::PostLoad, slot 0x{:X}, calls it at +0x{:X})", Displacement / 8, Slot, Call - Finder::ImageBase());
                    return Displacement / 8;
                }
            }
        }
        Log::Warning(L"Finder: UNavCollisionBase::Setup not found (UStaticMesh::PostLoad's navigation collision): new navigation collisions are left as made");
        return -1;
    }();
    return Index;
}
