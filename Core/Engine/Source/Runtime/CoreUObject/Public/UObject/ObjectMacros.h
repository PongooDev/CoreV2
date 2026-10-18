#pragma once

#include <cstdint>

enum EObjectFlags : int32_t
{
    RF_NoFlags = 0x00000000,
    RF_Public = 0x00000001,
    RF_Standalone = 0x00000002,
    RF_MarkAsNative = 0x00000004,
    RF_Transactional = 0x00000008,
    RF_ClassDefaultObject = 0x00000010,
    RF_ArchetypeObject = 0x00000020,
    RF_Transient = 0x00000040,
};

enum class EInternalObjectFlags : int32_t
{
    None = 0,
    Unreachable = 1 << 28,
    PendingKill = 1 << 29,
    RootSet = 1 << 30,
};

enum EFunctionFlags : uint32_t
{
    FUNC_Final = 0x00000001,
    FUNC_Net = 0x00000040,
    FUNC_NetReliable = 0x00000080,
    FUNC_Exec = 0x00000200,
    FUNC_Native = 0x00000400,
    FUNC_Event = 0x00000800,
    FUNC_Static = 0x00002000,
    FUNC_NetMulticast = 0x00004000,
    FUNC_Public = 0x00020000,
    FUNC_NetServer = 0x00200000,
    FUNC_NetClient = 0x01000000,
    FUNC_BlueprintCallable = 0x04000000,
    FUNC_BlueprintEvent = 0x08000000,
};

enum class EClassCastFlags : uint64_t
{
    None = 0x0000000000000000,
    Field = 0x0000000000000001,
    Enum = 0x0000000000000004,
    Struct = 0x0000000000000008,
    ScriptStruct = 0x0000000000000010,
    Class = 0x0000000000000020,
    ByteProperty = 0x0000000000000040,
    IntProperty = 0x0000000000000080,
    FloatProperty = 0x0000000000000100,
    ClassProperty = 0x0000000000000400,
    NameProperty = 0x0000000000002000,
    StrProperty = 0x0000000000004000,
    Property = 0x0000000000008000,
    ObjectProperty = 0x0000000000010000,
    BoolProperty = 0x0000000000020000,
    Function = 0x0000000000080000,
    StructProperty = 0x0000000000100000,
    ArrayProperty = 0x0000000000200000,
    ObjectPropertyBase = 0x0000000004000000,
    Package = 0x0000000400000000,
    Level = 0x0000000800000000,
    Actor = 0x0000001000000000,
    PlayerController = 0x0000002000000000,
    Pawn = 0x0000004000000000,
};

constexpr bool HasAnyCastFlags(EClassCastFlags Flags, EClassCastFlags Test)
{
    return (static_cast<uint64_t>(Flags) & static_cast<uint64_t>(Test)) != 0;
}
