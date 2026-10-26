#include "pch.h"
#include "ObjectLayout.h"
#include "Log.h"
#include "UObject/Class.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectGlobals.h"

#include <atomic>
#include <mutex>

namespace
{
    template <typename T>
    T Read(const void* Object, int32_t Offset)
    {
        return *reinterpret_cast<const T*>(reinterpret_cast<uintptr_t>(Object) + Offset);
    }

    const UProperty* FindProperty(const UObject* Struct, const wchar_t* Name)
    {
        return Struct ? static_cast<const UStruct*>(Struct)->FindPropertyByName(FName(Name, FNAME_Find)) : nullptr;
    }

    // From CoreUObject's own structs, which exist before any other. FBox::Min is a UStructProperty, so the first slot past
    // UProperty's flags that points at FVector is where each property class's own members start: UProperty's size (1.7.2's
    // UClassProperty constructor, 0x9AB690, sets PropertyClass at +0x70 where 1.2's, 0x1408D1690, sets +0x78). FVector's
    // X, Y and Z are at 0, 4 and 8, which only Offset_Internal holds (1.7.2's UProperty constructor, 0x13A66A0, stores the
    // offset at +0x44, 1.2's, 0x14121F0A0, at +0x50).
    bool DetectPropertyLayout(FObjectLayout& Layout)
    {
        const UObject* Vector = StaticFindObject(nullptr, nullptr, L"/Script/CoreUObject.Vector", false);
        const UObject* Box = StaticFindObject(nullptr, nullptr, L"/Script/CoreUObject.Box", false);
        const UProperty* Min = FindProperty(Box, L"Min");
        const UProperty* X = FindProperty(Vector, L"X");
        const UProperty* Y = FindProperty(Vector, L"Y");
        const UProperty* Z = FindProperty(Vector, L"Z");
        if (!Min || !X || !Y || !Z || !Min->GetClass()->HasAnyCastFlag(CASTCLASS_UStructProperty))
        {
            return false;
        }

        const int32_t First = Layout.UProperty_PropertyFlags + static_cast<int32_t>(sizeof(uint64_t));
        int32_t Size = -1;
        for (int32_t Offset = First; Offset < 0x100 && Size < 0; Offset += static_cast<int32_t>(sizeof(void*)))
        {
            if (Read<const UObject*>(Min, Offset) == Vector)
            {
                Size = Offset;
            }
        }

        int32_t OffsetInternal = -1;
        int32_t Matches = 0;
        for (int32_t Offset = First; Size > 0 && Offset + static_cast<int32_t>(sizeof(int32_t)) <= Size; Offset += static_cast<int32_t>(sizeof(int32_t)))
        {
            if (Read<int32_t>(X, Offset) == 0 && Read<int32_t>(Y, Offset) == 4 && Read<int32_t>(Z, Offset) == 8)
            {
                OffsetInternal = Offset;
                ++Matches;
            }
        }

        if (Matches != 1)
        {
            return false;
        }

        Layout.UProperty_Offset_Internal = OffsetInternal;
        Layout.UObjectPropertyBase_PropertyClass = Size;
        Layout.UStructProperty_Struct = Size;
        Layout.UArrayProperty_Inner = Size;
        Layout.UBoolProperty_FieldSize = Size;
        Layout.UBoolProperty_ByteOffset = Size + 1;
        Layout.UBoolProperty_ByteMask = Size + 2;
        Layout.UBoolProperty_FieldMask = Size + 3;
        return true;
    }
}

const FObjectLayout& ObjectLayout::Properties()
{
    static std::atomic<bool> bDetected = false;
    if (!bDetected.load(std::memory_order_acquire))
    {
        static std::mutex Mutex;
        static bool bReported = false;
        std::lock_guard Lock(Mutex);
        if (bDetected.load(std::memory_order_relaxed))
        {
            return Current;
        }

        // A failure keeps 1.1's layout and tries again on the next read, in case CoreUObject's structs weren't there yet.
        if (DetectPropertyLayout(Current))
        {
            Log::Info(L"Object layout: UProperty is 0x{:X} bytes, Offset_Internal at +0x{:X}", Current.UStructProperty_Struct, Current.UProperty_Offset_Internal);
            bDetected.store(true, std::memory_order_release);
        }
        else if (!bReported)
        {
            Log::Error(L"Object layout: UProperty's layout wasn't found from FVector and FBox, using 1.1's");
            bReported = true;
        }
    }
    return Current;
}
