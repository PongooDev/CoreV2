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
    RF_MarkAsRootSet = 0x00000080,
    RF_TagGarbageTemp = 0x00000100,
    RF_NeedInitialization = 0x00000200,
    RF_NeedLoad = 0x00000400,
    RF_KeepForCooker = 0x00000800,
    RF_NeedPostLoad = 0x00001000,
    RF_NeedPostLoadSubobjects = 0x00002000,
    RF_NewerVersionExists = 0x00004000,
    RF_BeginDestroyed = 0x00008000,
    RF_FinishDestroyed = 0x00010000,
    RF_BeingRegenerated = 0x00020000,
    RF_DefaultSubObject = 0x00040000,
    RF_WasLoaded = 0x00080000,
    RF_TextExportTransient = 0x00100000,
    RF_LoadCompleted = 0x00200000,
    RF_InheritableComponentTemplate = 0x00400000,
    RF_DuplicateTransient = 0x00800000,
    RF_StrongRefOnFrame = 0x01000000,
    RF_NonPIEDuplicateTransient = 0x02000000,
    RF_Dynamic = 0x04000000,
};

inline EObjectFlags operator|(EObjectFlags Left, EObjectFlags Right)
{
    return static_cast<EObjectFlags>(static_cast<int32_t>(Left) | static_cast<int32_t>(Right));
}

inline EObjectFlags operator&(EObjectFlags Left, EObjectFlags Right)
{
    return static_cast<EObjectFlags>(static_cast<int32_t>(Left) & static_cast<int32_t>(Right));
}

inline EObjectFlags operator~(EObjectFlags Flags)
{
    return static_cast<EObjectFlags>(~static_cast<int32_t>(Flags));
}

enum class EInternalObjectFlags : int32_t
{
    None = 0,
    Unreachable = 1 << 28,
    PendingKill = 1 << 29,
    RootSet = 1 << 30,
};

enum EFunctionFlags : uint32_t
{
    FUNC_None = 0x00000000,
    FUNC_Final = 0x00000001,
    FUNC_RequiredAPI = 0x00000002,
    FUNC_BlueprintAuthorityOnly = 0x00000004,
    FUNC_BlueprintCosmetic = 0x00000008,
    FUNC_Net = 0x00000040,
    FUNC_NetReliable = 0x00000080,
    FUNC_NetRequest = 0x00000100,
    FUNC_Exec = 0x00000200,
    FUNC_Native = 0x00000400,
    FUNC_Event = 0x00000800,
    FUNC_NetResponse = 0x00001000,
    FUNC_Static = 0x00002000,
    FUNC_NetMulticast = 0x00004000,
    FUNC_MulticastDelegate = 0x00010000,
    FUNC_Public = 0x00020000,
    FUNC_Private = 0x00040000,
    FUNC_Protected = 0x00080000,
    FUNC_Delegate = 0x00100000,
    FUNC_NetServer = 0x00200000,
    FUNC_HasOutParms = 0x00400000,
    FUNC_HasDefaults = 0x00800000,
    FUNC_NetClient = 0x01000000,
    FUNC_DLLImport = 0x02000000,
    FUNC_BlueprintCallable = 0x04000000,
    FUNC_BlueprintEvent = 0x08000000,
    FUNC_BlueprintPure = 0x10000000,
    FUNC_Const = 0x40000000,
    FUNC_NetValidate = 0x80000000,
};

enum EClassCastFlags : uint64_t
{
    CASTCLASS_None = 0x0000000000000000,
    CASTCLASS_UField = 0x0000000000000001,
    CASTCLASS_UInt8Property = 0x0000000000000002,
    CASTCLASS_UEnum = 0x0000000000000004,
    CASTCLASS_UStruct = 0x0000000000000008,
    CASTCLASS_UScriptStruct = 0x0000000000000010,
    CASTCLASS_UClass = 0x0000000000000020,
    CASTCLASS_UByteProperty = 0x0000000000000040,
    CASTCLASS_UIntProperty = 0x0000000000000080,
    CASTCLASS_UFloatProperty = 0x0000000000000100,
    CASTCLASS_UUInt64Property = 0x0000000000000200,
    CASTCLASS_UClassProperty = 0x0000000000000400,
    CASTCLASS_UUInt32Property = 0x0000000000000800,
    CASTCLASS_UInterfaceProperty = 0x0000000000001000,
    CASTCLASS_UNameProperty = 0x0000000000002000,
    CASTCLASS_UStrProperty = 0x0000000000004000,
    CASTCLASS_UProperty = 0x0000000000008000,
    CASTCLASS_UObjectProperty = 0x0000000000010000,
    CASTCLASS_UBoolProperty = 0x0000000000020000,
    CASTCLASS_UUInt16Property = 0x0000000000040000,
    CASTCLASS_UFunction = 0x0000000000080000,
    CASTCLASS_UStructProperty = 0x0000000000100000,
    CASTCLASS_UArrayProperty = 0x0000000000200000,
    CASTCLASS_UInt64Property = 0x0000000000400000,
    CASTCLASS_UDelegateProperty = 0x0000000000800000,
    CASTCLASS_UNumericProperty = 0x0000000001000000,
    CASTCLASS_UMulticastDelegateProperty = 0x0000000002000000,
    CASTCLASS_UObjectPropertyBase = 0x0000000004000000,
    CASTCLASS_UWeakObjectProperty = 0x0000000008000000,
    CASTCLASS_ULazyObjectProperty = 0x0000000010000000,
    CASTCLASS_UAssetObjectProperty = 0x0000000020000000,
    CASTCLASS_UTextProperty = 0x0000000040000000,
    CASTCLASS_UInt16Property = 0x0000000080000000,
    CASTCLASS_UDoubleProperty = 0x0000000100000000,
    CASTCLASS_UAssetClassProperty = 0x0000000200000000,
    CASTCLASS_UPackage = 0x0000000400000000,
    CASTCLASS_ULevel = 0x0000000800000000,
    CASTCLASS_AActor = 0x0000001000000000,
    CASTCLASS_APlayerController = 0x0000002000000000,
    CASTCLASS_APawn = 0x0000004000000000,
    CASTCLASS_USceneComponent = 0x0000008000000000,
    CASTCLASS_UPrimitiveComponent = 0x0000010000000000,
    CASTCLASS_USkinnedMeshComponent = 0x0000020000000000,
    CASTCLASS_USkeletalMeshComponent = 0x0000040000000000,
    CASTCLASS_UBlueprint = 0x0000080000000000,
    CASTCLASS_UDelegateFunction = 0x0000100000000000,
    CASTCLASS_UStaticMeshComponent = 0x0000200000000000,
    CASTCLASS_UMapProperty = 0x0000400000000000,
    CASTCLASS_USetProperty = 0x0000800000000000,
    CASTCLASS_UEnumProperty = 0x0001000000000000,
};

#define CPF_Parm 0x0000000000000080ull
